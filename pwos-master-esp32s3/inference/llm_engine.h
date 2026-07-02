/* ========================================================================
 * LLM 推理引擎 —— 公共头文件
 *
 * 基于: https://github.com/karpathy/llama2.c
 * 适配: ESP-IDF (FreeRTOS + PSRAM + 双核并行)
 * ======================================================================== */

#ifndef PWOS_LLM_ENGINE_H
#define PWOS_LLM_ENGINE_H

#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * 第一节: 基础类型
 * ======================================================================== */

/** 统一浮点类型 (ESP32 上等价于 float) */
typedef float v4sf;

/* ========================================================================
 * 第二节: 模型超参数
 * ======================================================================== */

typedef struct {
    int dim;          /* Transformer 隐藏维度 (d_model)           */
    int hidden_dim;   /* FFN 中间层维度                          */
    int n_layers;     /* Transformer 层数                        */
    int n_heads;      /* Query 注意力头数                        */
    int n_kv_heads;   /* Key/Value 头数 (≤ n_heads, 支持 GQA)    */
    int vocab_size;   /* 词表大小                                */
    int seq_len;      /* 最大序列长度 (决定 KV cache 内存占用)    */
} Config;

/* ========================================================================
 * 第三节: 模型权重 (映射自 checkpoint 文件)
 * ======================================================================== */

typedef struct {
    v4sf *token_embedding_table;  /* (vocab_size, dim)                */
    v4sf *rms_att_weight;         /* (n_layers, dim)  Attention RMSNorm */
    v4sf *rms_ffn_weight;         /* (n_layers, dim)  FFN RMSNorm       */
    v4sf *wq;                     /* (n_layers, dim, dim)   Q 投影      */
    v4sf *wk;                     /* (n_layers, dim, kv_dim) K 投影     */
    v4sf *wv;                     /* (n_layers, dim, kv_dim) V 投影     */
    v4sf *wo;                     /* (n_layers, dim, dim)   Attention 输出投影 */
    v4sf *w1;                     /* (n_layers, hidden_dim, dim) FFN 门 */
    v4sf *w2;                     /* (n_layers, dim, hidden_dim) FFN 下投影 */
    v4sf *w3;                     /* (n_layers, hidden_dim, dim) FFN 上投影 */
    v4sf *rms_final_weight;       /* (dim,)  最终 RMSNorm               */
    v4sf *wcls;                   /* (vocab_size, dim)  分类头 (可与 embedding 共享) */
} TransformerWeights;

/* ========================================================================
 * 第四节: 运行时状态 (激活缓冲区 + KV Cache)
 * ======================================================================== */

typedef struct {
    /* ---- 当前激活 ---- */
    v4sf *x;          /* (dim,)        主残差流                    */
    v4sf *xb;         /* (dim,)        残差分支临时缓冲             */
    v4sf *xb2;        /* (dim,)        Attention 输出临时缓冲       */
    v4sf *hb;         /* (hidden_dim,) FFN 隐藏层缓冲              */
    v4sf *hb2;        /* (hidden_dim,) FFN 门控临时缓冲            */
    v4sf *q;          /* (dim,)        Query 向量                  */
    v4sf *k;          /* (kv_dim,)     当前 Key (指向 key_cache)   */
    v4sf *v;          /* (kv_dim,)     当前 Value (指向 value_cache) */
    v4sf *att;        /* (n_heads, seq_len)  注意力分数矩阵        */
    v4sf *logits;     /* (vocab_size,) 输出 logits                 */

    /* ---- KV Cache (内存大户) ---- */
    v4sf *key_cache;    /* (n_layers, seq_len, kv_dim) */
    v4sf *value_cache;  /* (n_layers, seq_len, kv_dim) */
} RunState;

/* ========================================================================
 * 第五节: 分词器
 * ======================================================================== */

typedef struct {
    const char *str;
    int         id;
} TokenIndex;

typedef struct {
    char         **vocab;                        /* 词条字符串数组          */
    v4sf          *vocab_scores;                 /* 词条分数                */
    TokenIndex    *sorted_vocab;                 /* 排序索引 (二分查找用)    */
    int            vocab_size;                   /* 词表大小                */
    unsigned int   max_token_length;             /* 最长词条长度            */
    unsigned char  byte_pieces[512];             /* 单字节 → 字符串映射表   */
} Tokenizer;

/* ========================================================================
 * 第六节: 采样器
 * ======================================================================== */

typedef struct {
    float prob;         /* 概率值     */
    int   index;        /* token 索引 */
} ProbIndex;

typedef struct {
    int                 vocab_size;    /* 词表大小               */
    ProbIndex           *probindex;     /* top-p 采样排序缓冲区    */
    float               temperature;   /* 温度参数               */
    float               topp;          /* nucleus 采样阈值        */
    unsigned long long  rng_state;     /* xorshift 随机数状态     */
} Sampler;

/* ========================================================================
 * 第七节: Transformer 主结构
 * ======================================================================== */

typedef struct {
    Config             config;      /* 模型超参数               */
    TransformerWeights weights;     /* 权重指针                 */
    RunState           state;       /* 运行时缓冲区              */
    int                fd;          /* 文件描述符 (兼容 mmap)    */
    v4sf              *data;        /* 模型数据基址              */
    size_t             file_size;   /* checkpoint 文件字节数     */
} Transformer;

/* ========================================================================
 * 第八节: 回调类型 & 公开 API
 * ======================================================================== */

/** 每生成一个 token 时调用 */
typedef void (*generated_piece_cb)(void *ctx, const char *piece);

/** 生成完成时调用 (含吞吐统计) */
typedef void (*generated_complete_cb)(void *ctx, float tokens_per_second);

/* ---- 构建 & 释放 ---- */

void build_transformer(Transformer *t, char *checkpoint_path);
void free_transformer(Transformer *t);

void build_tokenizer(Tokenizer *t, char *tokenizer_path, int vocab_size);
void free_tokenizer(Tokenizer *t);

void build_sampler(Sampler *sampler, int vocab_size,
                   float temperature, float topp,
                   unsigned long long rng_seed);
void free_sampler(Sampler *sampler);

/* ---- 推理 ---- */

/**
 * 自回归文本生成。
 * @param transformer  已构建的 Transformer
 * @param tokenizer    已构建的分词器
 * @param sampler      已构建的采样器
 * @param prompt       输入提示 (NULL = 空字符串)
 * @param steps        最大生成步数 (受限于 seq_len)
 * @param cb_piece     每 token 回调 (可为 NULL)
 * @param cb_done      完成回调 (可为 NULL)
 * @param cb_ctx       回调上下文
 */
void generate(Transformer *transformer,
              Tokenizer   *tokenizer,
              Sampler     *sampler,
              char        *prompt,
              int          steps,
              generated_piece_cb    cb_piece,
              generated_complete_cb cb_done,
              void        *cb_ctx);

/* ---- 工具 ---- */

/** 从 stdin 读取一行 (CLI 调试用) */
void read_stdin(const char *guide, char *buffer, size_t bufsize);

#endif /* PWOS_LLM_ENGINE_H */
