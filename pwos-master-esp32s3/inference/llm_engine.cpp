/* ========================================================================
 * LLM 推理引擎 —— 基于 llama2.c 移植到 ESP32 平台
 *
 * 原始作者: https://github.com/karpathy/llama2.c
 * 适配: ESP-IDF (FreeRTOS + PSRAM + 双核并行)
 * ======================================================================== */

#include "llm_engine.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ========================================================================
 * 第一节: 配置常量
 * ======================================================================== */

/* ---- 双核同步位掩码 ---- */
#define TASK_0_BIT      (1 << 0)
#define TASK_1_BIT      (1 << 1)
#define FORWARD_TASK_1  (1 << 2)
#define FORWARD_TASK_2  (1 << 3)
#define ALL_SYNC_BITS      (TASK_0_BIT | TASK_1_BIT)
#define ALL_FORWARD_TASKS  (FORWARD_TASK_1 | FORWARD_TASK_2)

/* ---- 同步超时 (防止死锁 → 主动重启) ---- */
#define MATMUL_SYNC_TIMEOUT_MS   2000u
#define FORWARD_SYNC_TIMEOUT_MS  5000u

/* ---- 内存回退策略: 超过此阈值的分配不尝试内部 SRAM ---- */
#define SAFE_ALLOC_INTERNAL_FALLBACK_LIMIT  (32u * 1024u)

/* ---- 平台兼容 (原 llama2.c 使用 mmap) ---- */
#define MAP_FAILED  NULL
#define munmap(p, l)  custom_munmap(p)
#define close(fd)     custom_close(fd)

/* ========================================================================
 * 第二节: 任务参数结构体
 * ======================================================================== */

typedef struct {
    v4sf *xout, *x, *w;
    int   start, end;      /* 本任务负责的行范围 [start, end) */
    int   n, d;            /* 矩阵维度: w[d][n] × x[n] → xout[d] */
    int   task_num;        /* 事件组同步位 */
} MatMulTaskParams;

typedef struct {
    RunState          *s;
    TransformerWeights *w;
    Config            *p;
    int pos, start, loff, end;
    int dim, kv_dim, kv_mul, hidden_dim, head_size;
    int task_num;
} ForwardTaskParams;

/* ========================================================================
 * 第三节: 全局同步对象 & 任务句柄
 * ======================================================================== */

static const char *TAG = "LLM";

static EventGroupHandle_t xEventGroup;
static EventGroupHandle_t ForwardEventGroup;
static SemaphoreHandle_t  semaDataReady;
static SemaphoreHandle_t  semaForwardDataReady;

static TaskHandle_t handle_forward_task;
static TaskHandle_t matmul_task_2;

static ForwardTaskParams *forward_params;
static MatMulTaskParams  *matmul_params;

/* ---- 任务函数声明 ---- */
static void matmul_task(void *params);
static void forward_task(void *params);

/* ========================================================================
 * 第四节: 内存管理
 * ======================================================================== */

/**
 * 安全内存分配: PSRAM 优先，小块允许内部 SRAM 回退。
 * 失败时打印详细诊断并 abort()。
 */
template <typename T>
static T *safe_alloc(size_t num_elements)
{
    size_t total_size    = num_elements * sizeof(T);
    size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* 大块分配提前告警 */
    if (total_size > free_psram / 2u) {
        ESP_LOGW(TAG, "大块分配 %.1f KB, PSRAM 空闲 %.1f KB",
                 (double)total_size / 1024.0, (double)free_psram / 1024.0);
    }

    /* 优先 PSRAM */
    T *ptr = (T *)heap_caps_calloc(num_elements, sizeof(T), MALLOC_CAP_SPIRAM);

    /* 小块允许回退到内部 SRAM */
    if (!ptr && total_size <= SAFE_ALLOC_INTERNAL_FALLBACK_LIMIT) {
        ESP_LOGW(TAG, "PSRAM 不足, %.1f KB 回退到内部 SRAM",
                 (double)total_size / 1024.0);
        ptr = (T *)heap_caps_calloc(num_elements, sizeof(T), MALLOC_CAP_INTERNAL);
    }

    if (!ptr) {
        ESP_LOGE(TAG, "内存分配失败! 申请=%.1f KB PSRAM空闲=%.1f KB 内部空闲=%.1f KB",
                 (double)total_size / 1024.0,
                 (double)free_psram / 1024.0,
                 (double)free_internal / 1024.0);
        abort();
    }
    return ptr;
}

/* ---- 平台兼容: 用 free 替代 munmap ---- */
static void custom_munmap(void *ptr) { free(ptr); }
static int  custom_close(int fd)     { (void)fd; return 0; }

/* ========================================================================
 * 第五节: 模型加载 & 构建
 * ======================================================================== */

/**
 * 分配 RunState 中的所有激活缓冲区。
 * KV cache 是内存大户，单独打印大小便于诊断。
 */
static void malloc_run_state(RunState *s, const Config *p)
{
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    size_t kv_elems = (size_t)p->n_layers * (size_t)p->seq_len * (size_t)kv_dim;

    ESP_LOGI(TAG, "KV cache 大小: %.1f KB (layers=%d seq=%d kv_dim=%d)",
             (double)(kv_elems * sizeof(v4sf)) / 1024.0,
             p->n_layers, p->seq_len, kv_dim);

    s->x           = safe_alloc<v4sf>(p->dim);
    s->xb          = safe_alloc<v4sf>(p->dim);
    s->xb2         = safe_alloc<v4sf>(p->dim);
    s->hb          = safe_alloc<v4sf>(p->hidden_dim);
    s->hb2         = safe_alloc<v4sf>(p->hidden_dim);
    s->q           = safe_alloc<v4sf>(p->dim);
    s->key_cache   = safe_alloc<v4sf>(kv_elems);
    s->value_cache = safe_alloc<v4sf>(kv_elems);
    s->att         = safe_alloc<v4sf>((size_t)p->n_heads * (size_t)p->seq_len);
    s->logits      = safe_alloc<v4sf>(p->vocab_size);
}

/**
 * 将连续内存块按模型结构映射到 TransformerWeights 各字段。
 * 跳过文件中的 RoPE 频率数据 (ESP32 上实时计算)。
 */
static void memory_map_weights(TransformerWeights *w, const Config *p,
                               v4sf *ptr, int shared_weights)
{
    int head_size = p->dim / p->n_heads;
    unsigned long long L = p->n_layers;  /* 64-bit 防止大模型溢出 */

    w->token_embedding_table = ptr;  ptr += p->vocab_size * p->dim;
    w->rms_att_weight        = ptr;  ptr += L * p->dim;
    w->wq                    = ptr;  ptr += L * p->dim * (p->n_heads   * head_size);
    w->wk                    = ptr;  ptr += L * p->dim * (p->n_kv_heads * head_size);
    w->wv                    = ptr;  ptr += L * p->dim * (p->n_kv_heads * head_size);
    w->wo                    = ptr;  ptr += L * (p->n_heads * head_size) * p->dim;
    w->rms_ffn_weight        = ptr;  ptr += L * p->dim;
    w->w1                    = ptr;  ptr += L * p->dim * p->hidden_dim;
    w->w2                    = ptr;  ptr += L * p->hidden_dim * p->dim;
    w->w3                    = ptr;  ptr += L * p->dim * p->hidden_dim;
    w->rms_final_weight      = ptr;  ptr += p->dim;
    /* 跳过 RoPE 频率表 (训练时预计算，推理时实时算) */
    ptr += p->seq_len * head_size / 2;   /* freq_cis_real */
    ptr += p->seq_len * head_size / 2;   /* freq_cis_imag */
    w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

/**
 * 从 Flash 文件系统读取模型 checkpoint。
 * Config → 权重数据 → 映射到 TransformerWeights。
 */
static void read_checkpoint(const char *checkpoint, Config *config,
                            TransformerWeights *weights,
                            int *fd, v4sf **data, size_t *file_size)
{
    FILE *file = fopen(checkpoint, "rb");
    if (!file) {
        ESP_LOGE(TAG, "无法打开模型文件 %s", checkpoint);
        exit(EXIT_FAILURE);
    }

    /* 读取配置头 */
    if (fread(config, sizeof(Config), 1, file) != 1) {
        ESP_LOGE(TAG, "读取 Config 失败");
        exit(EXIT_FAILURE);
    }

    /* vocab_size 为负数 = 权重不共享 (llama2.c 约定) */
    int shared_weights = config->vocab_size > 0 ? 1 : 0;
    config->vocab_size = abs(config->vocab_size);
    ESP_LOGI(TAG, "词表大小: %d", config->vocab_size);

    /* 获取文件大小 */
    fseek(file, 0, SEEK_END);
    *file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    ESP_LOGI(TAG, "模型文件: %zu 字节, 空闲内存: %lu 字节",
             *file_size, esp_get_free_heap_size());

    /* 读入 PSRAM */
    *data = safe_alloc<v4sf>(*file_size / sizeof(v4sf));
    if (fread(*data, 1, *file_size, file) != *file_size) {
        ESP_LOGE(TAG, "读取模型数据失败");
        exit(EXIT_FAILURE);
    }
    fclose(file);

    ESP_LOGI(TAG, "模型加载完成, 空闲内存: %lu", esp_get_free_heap_size());

    /* 跳过 Config 头，映射权重 */
    v4sf *weights_ptr = *data + sizeof(Config) / sizeof(v4sf);
    memory_map_weights(weights, config, weights_ptr, shared_weights);
}

/**
 * 构建 Transformer: 加载模型 → 分配 RunState → 创建双核并行任务。
 */
void build_transformer(Transformer *t, char *checkpoint_path)
{
    read_checkpoint(checkpoint_path, &t->config, &t->weights,
                    &t->fd, &t->data, &t->file_size);
    malloc_run_state(&t->state, &t->config);
    ESP_LOGI(TAG, "Transformer 构建完成");

    /* 初始化同步对象 */
    xEventGroup          = xEventGroupCreate();
    ForwardEventGroup    = xEventGroupCreate();
    semaDataReady        = xSemaphoreCreateBinary();
    semaForwardDataReady = xSemaphoreCreateBinary();

    /* 信号量初始计数置 0 (Give 后立即 Take) */
    xSemaphoreGive(semaDataReady);
    xSemaphoreTake(semaDataReady, portMAX_DELAY);
    xSemaphoreGive(semaForwardDataReady);
    xSemaphoreTake(semaForwardDataReady, portMAX_DELAY);

    /* 分配任务参数 (全局，双核共享) */
    matmul_params  = safe_alloc<MatMulTaskParams>(1);
    forward_params = safe_alloc<ForwardTaskParams>(1);

    /* 创建 Core-0 辅助任务 (栈 6KB，优先级 19) */
    xTaskCreatePinnedToCore(matmul_task,  "MatMul2",     6144,
                            matmul_params, 19, &matmul_task_2, 0);
    xTaskCreatePinnedToCore(forward_task, "ForwardTask", 6144,
                            forward_params, 19, &handle_forward_task, 0);
    ESP_LOGI(TAG, "FreeRTOS 辅助任务已创建");
}

/* ========================================================================
 * 第六节: 资源释放
 * ======================================================================== */

static void free_run_state(RunState *s)
{
    free(s->x); free(s->xb); free(s->xb2);
    free(s->hb); free(s->hb2); free(s->q);
    free(s->att); free(s->logits);
    free(s->key_cache); free(s->value_cache);
}

void free_transformer(Transformer *t)
{
    /* 先删任务，再删同步对象 (避免任务访问已释放的 semaphore) */
    if (matmul_task_2)       { vTaskDelete(matmul_task_2);       matmul_task_2       = NULL; }
    if (handle_forward_task) { vTaskDelete(handle_forward_task); handle_forward_task = NULL; }

    if (semaDataReady)        { vSemaphoreDelete(semaDataReady);        semaDataReady        = NULL; }
    if (semaForwardDataReady) { vSemaphoreDelete(semaForwardDataReady); semaForwardDataReady = NULL; }
    if (xEventGroup)          { vEventGroupDelete(xEventGroup);         xEventGroup          = NULL; }
    if (ForwardEventGroup)    { vEventGroupDelete(ForwardEventGroup);   ForwardEventGroup    = NULL; }

    free(matmul_params);  matmul_params  = NULL;
    free(forward_params); forward_params = NULL;

    if (t->data != MAP_FAILED) munmap(t->data, t->file_size);
    if (t->fd   != -1)         close(t->fd);
    free_run_state(&t->state);
}

/* ========================================================================
 * 第七节: 神经网络基础算子
 * ======================================================================== */

/** RMS 归一化: o = weight * (x / sqrt(mean(x²) + ε)) */
static void rmsnorm(v4sf *o, v4sf *x, const v4sf *weight, int size)
{
    v4sf ss = 0.0f;
    for (int j = 0; j < size; j++) ss += x[j] * x[j];
    ss = 1.0f / sqrtf(ss / size + 1e-5f);
    for (int j = 0; j < size; j++) o[j] = weight[j] * (ss * x[j]);
}

/** Softmax (就地): x[i] = exp(x[i] - max) / Σ exp */
static void softmax(v4sf *x, int size)
{
    v4sf max_val = x[0];
    for (int i = 1; i < size; i++)
        if (x[i] > max_val) max_val = x[i];

    v4sf sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    for (int i = 0; i < size; i++) x[i] /= sum;
}

/* ========================================================================
 * 第八节: 双核矩阵乘法 (Core-0 辅助任务 + 主任务各算一半)
 * ======================================================================== */

/** Core-0 上的矩阵乘法辅助任务 (无限循环，信号量驱动) */
static void matmul_task(void *params)
{
    MatMulTaskParams *p = (MatMulTaskParams *)params;
    for (;;) {
        if (xSemaphoreTake(semaDataReady, portMAX_DELAY) != pdTRUE) continue;

        for (int i = p->start; i < p->end; i++) {
            v4sf val = 0.0f;
            for (int j = 0; j < p->n; j++)
                val += p->w[i * p->n + j] * p->x[j];
            p->xout[i] = val;
        }

        xSemaphoreGive(semaDataReady);
        EventBits_t bits = xEventGroupSync(xEventGroup, p->task_num,
                                           ALL_SYNC_BITS,
                                           pdMS_TO_TICKS(MATMUL_SYNC_TIMEOUT_MS));
        if ((bits & ALL_SYNC_BITS) != ALL_SYNC_BITS) {
            ESP_LOGE(TAG, "matmul_task 同步超时，重启"); esp_restart();
        }
    }
}

/**
 * 矩阵乘法 (双核并行): 主任务做前一半行，Core-0 做后一半行。
 * @param xout 输出 [d]
 * @param x    输入向量 [n]
 * @param w    权重矩阵 [d][n]
 */
static void matmul(v4sf *xout, v4sf *x, v4sf *w, int n, int d)
{
    /* 写入参数 → 唤醒 Core-0 任务 */
    *matmul_params = (MatMulTaskParams){xout, x, w, d / 2, d, n, d, TASK_1_BIT};
    xSemaphoreGive(semaDataReady);

    /* 主任务: 计算前一半行 [0, d/2) */
    for (int i = 0; i < d / 2; i++) {
        v4sf val = 0.0f;
        for (int j = 0; j < n; j++) val += w[i * n + j] * x[j];
        xout[i] = val;
    }

    /* 等待 Core-0 完成后一半 */
    if (xSemaphoreTake(semaDataReady, pdMS_TO_TICKS(MATMUL_SYNC_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "matmul 等待 helper 超时，重启"); esp_restart();
    }
    EventBits_t bits = xEventGroupSync(xEventGroup, TASK_0_BIT, ALL_SYNC_BITS,
                                       pdMS_TO_TICKS(MATMUL_SYNC_TIMEOUT_MS));
    if ((bits & ALL_SYNC_BITS) != ALL_SYNC_BITS) {
        ESP_LOGE(TAG, "matmul 同步超时，重启"); esp_restart();
    }
    xEventGroupClearBits(xEventGroup, ALL_SYNC_BITS);
}

/* ========================================================================
 * 第九节: 注意力计算辅助任务 + Transformer 前向传播
 * ======================================================================== */

/** Core-0 上的注意力计算辅助任务 */
static void forward_task(void *params)
{
    ForwardTaskParams *p = (ForwardTaskParams *)params;
    for (;;) {
        if (xSemaphoreTake(semaForwardDataReady, portMAX_DELAY) != pdTRUE) continue;

        for (int h = p->start; h < p->end; h++) {
            v4sf *q   = p->s->q   + h * p->head_size;
            v4sf *att = p->s->att + h * p->p->seq_len;

            /* 计算 attention scores */
            for (int t = 0; t <= p->pos; t++) {
                v4sf *k = p->s->key_cache + p->loff
                        + t * p->kv_dim + (h / p->kv_mul) * p->head_size;
                v4sf score = 0.0f;
                for (int i = 0; i < p->head_size; i++) score += q[i] * k[i];
                att[t] = score / sqrtf((v4sf)p->head_size);
            }

            softmax(att, p->pos + 1);

            /* 加权求和 values */
            v4sf *xb = p->s->xb + h * p->head_size;
            memset(xb, 0, p->head_size * sizeof(v4sf));
            for (int t = 0; t <= p->pos; t++) {
                v4sf *v = p->s->value_cache + p->loff
                        + t * p->kv_dim + (h / p->kv_mul) * p->head_size;
                v4sf a = att[t];
                for (int i = 0; i < p->head_size; i++) xb[i] += a * v[i];
            }
        }

        xSemaphoreGive(semaForwardDataReady);
        EventBits_t bits = xEventGroupSync(ForwardEventGroup, p->task_num,
                                           ALL_FORWARD_TASKS,
                                           pdMS_TO_TICKS(FORWARD_SYNC_TIMEOUT_MS));
        if ((bits & ALL_FORWARD_TASKS) != ALL_FORWARD_TASKS) {
            ESP_LOGE(TAG, "forward_task 同步超时，重启"); esp_restart();
        }
    }
}

/**
 * Transformer 单步前向传播。
 * @return logits 数组指针 (RunState 内部，调用者不应释放)
 */
static v4sf *forward(Transformer *transformer, int token, int pos)
{
    Config            *p  = &transformer->config;
    TransformerWeights *w = &transformer->weights;
    RunState          *s  = &transformer->state;

    int dim        = p->dim;
    int kv_dim     = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul     = p->n_heads / p->n_kv_heads;
    int hidden_dim = p->hidden_dim;
    int head_size  = dim / p->n_heads;

    /* ---- Token Embedding ---- */
    memcpy(s->x, w->token_embedding_table + token * dim, dim * sizeof(v4sf));

    /* ---- 逐层前向 ---- */
    for (unsigned long long l = 0; l < p->n_layers; l++) {

        /* Attention RMSNorm */
        rmsnorm(s->xb, s->x, w->rms_att_weight + l * dim, dim);

        /* KV cache 写入位置 */
        int loff = l * p->seq_len * kv_dim;
        s->k = s->key_cache   + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;

        /* QKV 投影 */
        matmul(s->q, s->xb, w->wq + l * dim * dim,     dim, dim);
        matmul(s->k, s->xb, w->wk + l * dim * kv_dim,  dim, kv_dim);
        matmul(s->v, s->xb, w->wv + l * dim * kv_dim,  dim, kv_dim);

        /* ---- RoPE 旋转位置编码 ---- */
        for (int i = 0; i < dim; i += 2) {
            int head_dim = i % head_size;
            v4sf freq = 1.0f / powf(10000.0f, (v4sf)head_dim / (v4sf)head_size);
            v4sf fcr  = cosf(pos * freq);
            v4sf fci  = sinf(pos * freq);
            int rotn  = (i < kv_dim) ? 2 : 1;  /* q 和 k 都需要旋转; v 不需要 */
            for (int v = 0; v < rotn; v++) {
                v4sf *vec = (v == 0) ? s->q : s->k;
                v4sf v0 = vec[i], v1 = vec[i + 1];
                vec[i]     = v0 * fcr - v1 * fci;
                vec[i + 1] = v0 * fci + v1 * fcr;
            }
        }

        /* ---- 双核并行 Multi-Head Attention ---- */
        *forward_params = (ForwardTaskParams){
            .s = s, .w = w, .p = p, .pos = pos,
            .start = p->n_heads / 2, .loff = loff, .end = p->n_heads,
            .dim = dim, .kv_dim = kv_dim, .kv_mul = kv_mul,
            .hidden_dim = hidden_dim, .head_size = head_size,
            .task_num = FORWARD_TASK_1,
        };
        xSemaphoreGive(semaForwardDataReady);

        /* 主任务: 计算前一半注意力头 */
        for (int h = 0; h < p->n_heads / 2; h++) {
            v4sf *q   = s->q   + h * head_size;
            v4sf *att = s->att + h * p->seq_len;

            for (int t = 0; t <= pos; t++) {
                v4sf *k = s->key_cache + loff + t * kv_dim
                        + (h / kv_mul) * head_size;
                v4sf score = 0.0f;
                for (int i = 0; i < head_size; i++) score += q[i] * k[i];
                att[t] = score / sqrtf((v4sf)head_size);
            }
            softmax(att, pos + 1);

            v4sf *xb = s->xb + h * head_size;
            memset(xb, 0, head_size * sizeof(v4sf));
            for (int t = 0; t <= pos; t++) {
                v4sf *v = s->value_cache + loff + t * kv_dim
                        + (h / kv_mul) * head_size;
                v4sf a = att[t];
                for (int i = 0; i < head_size; i++) xb[i] += a * v[i];
            }
        }

        /* 等待 Core-0 完成后一半注意力头 */
        if (xSemaphoreTake(semaForwardDataReady,
                           pdMS_TO_TICKS(FORWARD_SYNC_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "forward 等待 helper 超时，重启"); esp_restart();
        }
        EventBits_t fwd_bits = xEventGroupSync(
            ForwardEventGroup, FORWARD_TASK_2, ALL_FORWARD_TASKS,
            pdMS_TO_TICKS(FORWARD_SYNC_TIMEOUT_MS));
        if ((fwd_bits & ALL_FORWARD_TASKS) != ALL_FORWARD_TASKS) {
            ESP_LOGE(TAG, "forward attention 同步超时，重启"); esp_restart();
        }
        xEventGroupClearBits(ForwardEventGroup, ALL_FORWARD_TASKS);

        /* ---- Attention 输出投影 + 残差连接 ---- */
        matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);
        for (int i = 0; i < dim; i++) s->x[i] += s->xb2[i];

        /* ---- FFN (SwiGLU) ---- */
        rmsnorm(s->xb, s->x, w->rms_ffn_weight + l * dim, dim);
        matmul(s->hb,  s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
        matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);

        for (int i = 0; i < hidden_dim; i++) {
            v4sf val = s->hb[i];
            val *= 1.0f / (1.0f + expf(-val));  /* SiLU */
            val *= s->hb2[i];                    /* × w3(x) */
            s->hb[i] = val;
        }

        matmul(s->xb, s->hb, w->w2 + l * dim * hidden_dim, hidden_dim, dim);
        for (int i = 0; i < dim; i++) s->x[i] += s->xb[i];
    }

    /* ---- 最终 RMSNorm + 分类头 ---- */
    rmsnorm(s->x, s->x, w->rms_final_weight, dim);
    matmul(s->logits, s->x, w->wcls, dim, p->vocab_size);
    return s->logits;
}

/* ========================================================================
 * 第十节: BPE 分词器
 * ======================================================================== */

static int compare_tokens(const void *a, const void *b) {
    return strcmp(((const TokenIndex *)a)->str, ((const TokenIndex *)b)->str);
}

void build_tokenizer(Tokenizer *t, char *tokenizer_path, int vocab_size)
{
    ESP_LOGI(TAG, "词表大小: %d", vocab_size);
    t->vocab_size   = vocab_size;
    t->vocab        = safe_alloc<char *>(vocab_size);
    t->vocab_scores = safe_alloc<v4sf>(vocab_size);
    t->sorted_vocab = NULL;  /* 延迟初始化 */

    /* 初始化单字节映射表: byte_pieces[i*2] = byte, [i*2+1] = '\0' */
    for (int i = 0; i < 256; i++) {
        t->byte_pieces[i * 2]     = (unsigned char)i;
        t->byte_pieces[i * 2 + 1] = '\0';
    }

    FILE *file = fopen(tokenizer_path, "rb");
    if (!file) { ESP_LOGE(TAG, "无法打开 %s", tokenizer_path); exit(EXIT_FAILURE); }

    if (fread(&t->max_token_length, sizeof(int), 1, file) != 1) {
        ESP_LOGE(TAG, "读取 max_token_length 失败"); exit(EXIT_FAILURE);
    }

    int len;
    for (int i = 0; i < vocab_size; i++) {
        if (fread(t->vocab_scores + i, sizeof(v4sf), 1, file) != 1 ||
            fread(&len, sizeof(int), 1, file) != 1) {
            ESP_LOGE(TAG, "读取词表条目 %d 失败", i); exit(EXIT_FAILURE);
        }
        t->vocab[i] = safe_alloc<char>(len + 1);
        if (fread(t->vocab[i], len, 1, file) != 1) {
            ESP_LOGE(TAG, "读取词条文本 %d 失败", i); exit(EXIT_FAILURE);
        }
        t->vocab[i][len] = '\0';
    }
    fclose(file);
    ESP_LOGI(TAG, "分词器构建完成");
}

void free_tokenizer(Tokenizer *t)
{
    for (int i = 0; i < t->vocab_size; i++) free(t->vocab[i]);
    free(t->vocab); free(t->vocab_scores); free(t->sorted_vocab);
}

/** 将 token 解码为字符串 (返回指针在 Tokenizer 内部，不用释放) */
char *decode(Tokenizer *t, int prev_token, int token)
{
    char *piece = t->vocab[token];
    /* BOS 后跳过前导空格 (SentencePiece 约定) */
    if (prev_token == 1 && piece[0] == ' ') piece++;

    /* 原始字节 token: <0xNN> → 单字节字符串 */
    unsigned char byte_val;
    if (sscanf(piece, "<0x%02hhX>", &byte_val) == 1)
        piece = (char *)t->byte_pieces + byte_val * 2;

    return piece;
}

/** 安全打印 token 字符串 (过滤不可打印控制字符) */
static void safe_printf(const char *piece)
{
    if (!piece || !piece[0]) return;
    if (piece[1] == '\0') {
        unsigned char c = (unsigned char)piece[0];
        if (!(isprint(c) || isspace(c))) return;
    }
    printf("%s", piece);
}

/** 在排序词表中二分查找字符串，返回 token id 或 -1 */
static int str_lookup(const char *str, const TokenIndex *sorted_vocab, int vocab_size)
{
    TokenIndex key = { .str = str, .id = 0 };
    TokenIndex *res = (TokenIndex *)bsearch(&key, sorted_vocab, vocab_size,
                                            sizeof(TokenIndex), compare_tokens);
    return res ? res->id : -1;
}

/**
 * BPE 编码: 将 UTF-8 文本转换为 token 序列。
 * tokens[] 由调用者预分配 (上界: strlen(text) + 3)。
 */
void encode(Tokenizer *t, char *text, int8_t bos, int8_t eos,
            int *tokens, int *n_tokens)
{
    if (!text) { ESP_LOGE(TAG, "encode: text 为 NULL"); exit(EXIT_FAILURE); }

    /* 延迟初始化排序词表 */
    if (!t->sorted_vocab) {
        t->sorted_vocab = safe_alloc<TokenIndex>(t->vocab_size);
        for (int i = 0; i < t->vocab_size; i++) {
            t->sorted_vocab[i].str = t->vocab[i];
            t->sorted_vocab[i].id  = i;
        }
        qsort(t->sorted_vocab, t->vocab_size, sizeof(TokenIndex), compare_tokens);
    }

    /* BPE 合并缓冲区 */
    const size_t str_buf_size = (size_t)t->max_token_length * 2u + 3u;
    char *str_buffer = safe_alloc<char>(str_buf_size);

    *n_tokens = 0;
    if (bos) tokens[(*n_tokens)++] = 1;               /* BOS */
    if (text[0]) tokens[(*n_tokens)++] = str_lookup(" ", t->sorted_vocab, t->vocab_size);  /* dummy prefix */

    /* ---- UTF-8 字节级编码 ---- */
    size_t str_len = 0;
    for (char *c = text; *c; c++) {
        /* 前导字节 / ASCII → 重置缓冲区 */
        if ((*c & 0xC0) != 0x80) str_len = 0;

        str_buffer[str_len++] = *c;
        str_buffer[str_len]   = '\0';

        /* 继续累积 UTF-8 后续字节 */
        if ((*(c + 1) & 0xC0) == 0x80 && str_len < 4) continue;

        int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
        if (id != -1) {
            tokens[(*n_tokens)++] = id;
        } else {
            /* 回退: 逐字节编码 (+3 跳过 <unk>, <s>, </s>) */
            for (size_t i = 0; i < str_len; i++)
                tokens[(*n_tokens)++] = (unsigned char)str_buffer[i] + 3;
        }
        str_len = 0;
    }

    /* ---- BPE 合并循环 ---- */
    for (;;) {
        v4sf best_score = -1e10f;
        int  best_id = -1, best_idx = -1;

        for (int i = 0; i < *n_tokens - 1; i++) {
            int w = snprintf(str_buffer, str_buf_size, "%s%s",
                             t->vocab[tokens[i]], t->vocab[tokens[i + 1]]);
            if (w < 0 || (size_t)w >= str_buf_size) continue;  /* token 太长，跳过 */

            int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
            if (id != -1 && t->vocab_scores[id] > best_score) {
                best_score = t->vocab_scores[id];
                best_id = id; best_idx = i;
            }
        }
        if (best_idx == -1) break;

        /* 合并 best_idx 和 best_idx+1 → best_id */
        tokens[best_idx] = best_id;
        for (int i = best_idx + 1; i < *n_tokens - 1; i++)
            tokens[i] = tokens[i + 1];
        (*n_tokens)--;
    }

    if (eos) tokens[(*n_tokens)++] = 2;  /* EOS */
    free(str_buffer);
}

/* ========================================================================
 * 第十一节: 采样器
 * ======================================================================== */

static int sample_argmax(const v4sf *probs, int n) {
    int max_i = 0;
    for (int i = 1; i < n; i++)
        if (probs[i] > probs[max_i]) max_i = i;
    return max_i;
}

static int sample_mult(const v4sf *probs, int n, v4sf coin) {
    v4sf cdf = 0.0f;
    for (int i = 0; i < n; i++) {
        cdf += probs[i];
        if (coin < cdf) return i;
    }
    return n - 1;
}

static int compare_prob(const void *a, const void *b) {
    float pa = ((const ProbIndex *)a)->prob;
    float pb = ((const ProbIndex *)b)->prob;
    return (pa > pb) ? -1 : (pa < pb) ? 1 : 0;
}

/** Top-P (nucleus) 采样 */
static int sample_topp(const v4sf *probs, int n, v4sf topp,
                       ProbIndex *probindex, v4sf coin)
{
    if (!probindex || n <= 0) return 0;

    /* 筛选概率 ≥ cutoff 的候选 */
    const v4sf cutoff = (1.0f - topp) / (n - 1);
    int n0 = 0;
    for (int i = 0; i < n; i++) {
        if (probs[i] >= cutoff) {
            probindex[n0].index = i;
            probindex[n0].prob  = probs[i];
            n0++;
        }
    }
    if (n0 == 0) return 0;

    /* 降序排列，累加到超过 topp */
    qsort(probindex, n0, sizeof(ProbIndex), compare_prob);
    v4sf cum = 0.0f;
    int last = n0 - 1;
    for (int i = 0; i < n0; i++) {
        cum += probindex[i].prob;
        if (cum > topp) { last = i; break; }
    }

    /* 从截断集合中采样 */
    v4sf r = coin * cum, cdf = 0.0f;
    for (int i = 0; i <= last; i++) {
        cdf += probindex[i].prob;
        if (r < cdf) return probindex[i].index;
    }
    return probindex[last].index;
}

void build_sampler(Sampler *sampler, int vocab_size, v4sf temperature,
                   v4sf topp, unsigned long long rng_seed)
{
    sampler->vocab_size  = vocab_size;
    sampler->temperature = temperature;
    sampler->topp        = topp;
    sampler->rng_state   = rng_seed;
    sampler->probindex   = safe_alloc<ProbIndex>(vocab_size);
    ESP_LOGI(TAG, "采样器构建完成");
}

void free_sampler(Sampler *sampler) { free(sampler->probindex); }

/* ---- xorshift 随机数生成器 ---- */
static unsigned int random_u32(unsigned long long *state) {
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return (unsigned int)((*state * 0x2545F4914F6CDD1Dull) >> 32);
}
static v4sf random_f32(unsigned long long *state) {
    return (v4sf)(random_u32(state) >> 8) / 16777216.0f;
}

/** 从 logits 中采样下一个 token */
int sample(Sampler *sampler, v4sf *logits)
{
    if (sampler->temperature == 0.0f)
        return sample_argmax(logits, sampler->vocab_size);

    /* 温度缩放 + Softmax */
    for (int q = 0; q < sampler->vocab_size; q++)
        logits[q] /= sampler->temperature;
    softmax(logits, sampler->vocab_size);

    v4sf coin = random_f32(&sampler->rng_state);
    if (sampler->topp <= 0.0f || sampler->topp >= 1.0f)
        return sample_mult(logits, sampler->vocab_size, coin);
    else
        return sample_topp(logits, sampler->vocab_size, sampler->topp,
                           sampler->probindex, coin);
}

/* ========================================================================
 * 第十二节: 文本生成循环
 * ======================================================================== */

static long time_in_ms(void) { return (long)(esp_timer_get_time() / 1000); }

void generate(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler,
              char *prompt, int steps,
              generated_piece_cb cb_piece, generated_complete_cb cb_done,
              void *cb_ctx)
{
    char empty[] = "";
    if (!prompt) prompt = empty;

    /* steps 上限 = seq_len (超过 KV cache 容量会越界) */
    if (steps <= 0 || steps > transformer->config.seq_len)
        steps = transformer->config.seq_len;

    /* ---- 编码 prompt ---- */
    size_t prompt_len = strlen(prompt);
    size_t max_tokens = prompt_len + 3u;
    if (max_tokens > 1024u) {
        ESP_LOGE(TAG, "prompt 过长 (%zu 字节)，拒绝", prompt_len);
        return;
    }
    int num_tokens = 0;
    int *prompt_tokens = safe_alloc<int>(max_tokens);
    encode(tokenizer, prompt, 1, 0, prompt_tokens, &num_tokens);
    if (num_tokens < 1) {
        ESP_LOGE(TAG, "prompt 编码失败 (0 token)"); exit(EXIT_FAILURE);
    }

    /* ---- 自回归生成 ---- */
    long start_ms = 0;
    int  token = prompt_tokens[0];
    int  pos   = 0;

    while (pos < steps) {
        vTaskDelay(1);  /* 喂狗 + 允许调度 */

        v4sf *logits = forward(transformer, token, pos);
        int next;

        if (pos < num_tokens - 1) {
            next = prompt_tokens[pos + 1];   /* prompt 阶段: 强制下一个 token */
        } else {
            next = sample(sampler, logits);  /* 生成阶段: 采样 */
        }
        pos++;

        /* EOS 回退保护: 模型过早输出 EOS 时重试，但不能退到负数 */
        if (next == 1) {
            if (pos > 0) { pos--; continue; }
            next = 0;  /* pos==0 无法回退，fallback 到 <unk> */
        }

        /* 输出 token */
        char *piece = decode(tokenizer, token, next);
        if (cb_piece) cb_piece(cb_ctx, piece);
        else          safe_printf(piece);
        fflush(stdout);
        token = next;

        if (!start_ms) start_ms = time_in_ms();
    }
    printf("\n");

    /* 吞吐统计 */
    if (pos > 1) {
        long end_ms = time_in_ms();
        float tks = (pos - 1) / (double)(end_ms - start_ms) * 1000.0f;
        fprintf(stderr, "achieved tok/s: %f\n", tks);
        if (cb_done) cb_done(cb_ctx, tks);
    }

    free(prompt_tokens);
    ESP_LOGI(TAG, "生成完成");
}

/* ========================================================================
 * 第十三节: 工具函数
 * ======================================================================== */

void read_stdin(const char *guide, char *buffer, size_t bufsize)
{
    printf("%s", guide);
    if (fgets(buffer, (int)bufsize, stdin)) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') buffer[len - 1] = '\0';
    }
}
