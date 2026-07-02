/* ========================================================================
 * 推理运行时 —— 公共头文件
 *
 * 管理 LLM 推理的完整生命周期:
 *   OFFLINE → LOADING → READY ⇄ QUEUED → RUNNING → DONE
 *                                   ↳ ERROR
 * ======================================================================== */

#ifndef PWOS_INFERENCE_RUNTIME_H
#define PWOS_INFERENCE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 缓冲区容量 ---- */
#define PWOS_INFERENCE_PROMPT_CAP  192u    /* 输入 prompt 最大字节数 */
#define PWOS_INFERENCE_OUTPUT_CAP  4096u   /* 输出文本最大字节数     */

/* ========================================================================
 * 第一节: 推理状态机
 * ======================================================================== */

typedef enum {
    PWOS_INFERENCE_OFFLINE = 0,  /* 未启动               */
    PWOS_INFERENCE_LOADING = 1,  /* 正在加载模型          */
    PWOS_INFERENCE_READY   = 2,  /* 就绪，等待请求         */
    PWOS_INFERENCE_QUEUED  = 3,  /* 请求已入队，等待执行    */
    PWOS_INFERENCE_RUNNING = 4,  /* 正在推理中             */
    PWOS_INFERENCE_DONE    = 5,  /* 推理完成，可接受新请求  */
    PWOS_INFERENCE_ERROR   = 6,  /* 错误 (PSRAM不足/挂载失败) */
} pwos_inference_state_t;

/* ========================================================================
 * 第二节: 运行时快照 (用于外部监控)
 * ======================================================================== */

typedef struct {
    uint8_t  initialized;        /* 是否已调用 start()                  */
    uint8_t  state;              /* 当前状态 (参见 pwos_inference_state_t) */
    uint32_t request_id;         /* 当前/上次请求 ID                     */
    uint16_t requested_steps;    /* 请求的生成步数                        */
    uint16_t generated_bytes;    /* 已生成文本字节数                      */
    uint32_t runs;               /* 累计完成推理次数                      */
    uint32_t rejected_busy;      /* 因忙被拒绝的请求数                    */
    uint32_t psram_total;        /* PSRAM 总量 (字节)                    */
    uint32_t psram_free;         /* PSRAM 当前空闲 (字节)                 */
    float    tokens_per_second;  /* 上轮推理吞吐                          */
    int32_t  last_error;         /* 最近错误码 (0 = 无错误)               */
} pwos_inference_snapshot_t;

/* ========================================================================
 * 第三节: 公开 API
 * ======================================================================== */

/**
 * 启动推理运行时 (幂等)。
 * 创建 worker 任务 (加载模型) 和 console 任务 (stdin 输入)。
 * @return 0=成功, -1=资源不足
 */
int pwos_inference_runtime_start(void);

/**
 * 提交推理请求 (非阻塞)。
 * @param prompt         输入文本 (null 结尾)
 * @param steps          最大生成步数
 * @param temperature    采样温度 (0 = 贪心)
 * @param topp           nucleus 采样阈值 (0~1, 超出范围 = 关闭)
 * @param out_request_id 输出: 分配的请求 ID
 * @return 0=成功, -1=参数无效, -2=模型未就绪/正忙
 */
int pwos_inference_runtime_submit(
    const char *prompt,
    uint16_t    steps,
    float       temperature,
    float       topp,
    uint32_t   *out_request_id);

/**
 * 获取运行时快照 (线程安全)。
 * @param out_snapshot 输出快照 (不可为 NULL)
 */
void pwos_inference_runtime_get_snapshot(
    pwos_inference_snapshot_t *out_snapshot);

/**
 * 读取推理结果 (分块读取，线程安全)。
 * @param request_id   请求 ID (需与当前请求匹配)
 * @param offset       读取偏移 (0 = 从头)
 * @param out          输出缓冲区
 * @param in_out_len   输入: 缓冲区大小; 输出: 实际拷贝字节数
 * @return 0=成功, -1=ID 不匹配/参数无效
 */
int pwos_inference_runtime_read_result(
    uint32_t  request_id,
    uint16_t  offset,
    uint8_t  *out,
    uint16_t *in_out_len);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_INFERENCE_RUNTIME_H */
