/* ========================================================================
 * 推理运行时 —— 实现
 *
 * 负责: SPIFFS 挂载 → 模型加载 → 请求队列 → 文本生成 → 结果回读
 *
 * 任务架构:
 *   llm_worker  (Core-1, prio 5, 栈 16KB) — 模型加载 + 推理执行
 *   llm_console (任意核, prio 3, 栈  4KB) — stdin 输入监控
 *
 * 线程安全: g_inference 所有字段通过 runtime_lock/runtime_unlock 保护
 * ======================================================================== */

#include "inference_runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "llm_engine.h"

/* ========================================================================
 * 第一节: 配置常量
 * ======================================================================== */

#define PWOS_INFERENCE_MODEL_PATH     "/model/stories260K.bin"   /* 模型文件路径   */
#define PWOS_INFERENCE_TOKENIZER_PATH "/model/tok512.bin"        /* 分词器文件路径  */
#define PWOS_INFERENCE_WORKER_STACK   16384u                     /* worker 任务栈   */
#define PWOS_INFERENCE_CONSOLE_STACK  4096u                      /* console 任务栈  */

/* ========================================================================
 * 第二节: 内部类型
 * ======================================================================== */

/** 推理请求 (队列元素) */
typedef struct {
    uint32_t request_id;                            /* 请求唯一 ID               */
    uint16_t steps;                                 /* 最大生成步数               */
    float    temperature;                           /* 采样温度                   */
    float    topp;                                  /* nucleus 采样阈值            */
    char     prompt[PWOS_INFERENCE_PROMPT_CAP];     /* 输入文本 (null 结尾)        */
} inference_request_t;

/** 推理运行时全局状态 (单例) */
typedef struct {
    Transformer            transformer;             /* LLM 模型                  */
    Tokenizer              tokenizer;               /* 分词器                     */
    Sampler                sampler;                 /* 采样器                     */
    QueueHandle_t          queue;                   /* 请求队列 (深度 1)           */
    SemaphoreHandle_t      mutex;                   /* 全局互斥锁                  */
    TaskHandle_t           worker_task;             /* 推理 worker 任务句柄        */
    TaskHandle_t           console_task;            /* stdin 控制台任务句柄        */
    char                  *output;                  /* 输出文本缓冲区 (PSRAM)       */
    pwos_inference_snapshot_t snapshot;             /* 状态快照                    */
} inference_runtime_t;

/* ========================================================================
 * 第三节: 全局状态
 * ======================================================================== */

static const char         *TAG = "pwos_inference";
static inference_runtime_t g_inference;             /* 零初始化 (BSS)             */

/* ========================================================================
 * 第四节: 同步原语
 * ======================================================================== */

/** 获取全局互斥锁 (阻塞) */
static void runtime_lock(void)
{
    if (g_inference.mutex) {
        (void)xSemaphoreTake(g_inference.mutex, portMAX_DELAY);
    }
}

/** 释放全局互斥锁 */
static void runtime_unlock(void)
{
    if (g_inference.mutex) {
        (void)xSemaphoreGive(g_inference.mutex);
    }
}

/* ========================================================================
 * 第五节: 输出回调 (供 generate() 使用)
 * ======================================================================== */

/** 判断 token 字符串是否可打印 */
static int printable_piece(const char *piece)
{
    if (!piece || !piece[0]) return 0;
    if (piece[1] == '\0') {
        unsigned char c = (unsigned char)piece[0];
        return isprint(c) || isspace(c);
    }
    return 1;
}

/**
 * generate() 的每 token 回调: 将生成文本追加到输出缓冲区。
 * 线程安全 (内部持锁)。
 */
static void output_piece(void *ctx, const char *piece)
{
    (void)ctx;
    if (!printable_piece(piece)) return;

    size_t piece_len = strlen(piece);

    runtime_lock();
    {
        /* 防御: generated_bytes 不应超过 OUTPUT_CAP-1，
         * 但若因上游 bug 导致越界，available 会回绕为极大值，
         * 此处做饱和处理防止 memcpy 写穿缓冲区。 */
        uint16_t gen = g_inference.snapshot.generated_bytes;
        size_t   available;

        if (gen >= PWOS_INFERENCE_OUTPUT_CAP) {
            runtime_unlock();
            return;  /* 缓冲区已满，静默丢弃 */
        }
        available = (size_t)(PWOS_INFERENCE_OUTPUT_CAP - 1u - gen);

        if (piece_len > available) piece_len = available;
        if (piece_len > 0u) {
            memcpy(g_inference.output + gen, piece, piece_len);
            g_inference.snapshot.generated_bytes = gen + (uint16_t)piece_len;
            g_inference.output[gen + piece_len] = '\0';
        }
    }
    runtime_unlock();

    /* 同时回显到串口 (调试用) */
    printf("%s", piece);
    fflush(stdout);
}

/**
 * generate() 的完成回调: 记录吞吐量统计。
 */
static void generation_done(void *ctx, float tokens_per_second)
{
    (void)ctx;
    runtime_lock();
    g_inference.snapshot.tokens_per_second = tokens_per_second;
    runtime_unlock();
}

/* ========================================================================
 * 第六节: SPIFFS 分区挂载
 * ======================================================================== */

/** 挂载存储模型文件的 SPIFFS 分区 */
static int mount_model_partition(void)
{
    esp_vfs_spiffs_conf_t config = {
        .base_path              = "/model",
        .partition_label        = "model",
        .max_files              = 3,
        .format_if_mount_failed = false,
    };
    size_t total = 0u, used = 0u;
    esp_err_t err;

    err = esp_vfs_spiffs_register(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "model 分区挂载失败: %s", esp_err_to_name(err));
        return (int)err;
    }
    err = esp_spiffs_info("model", &total, &used);
    if (err != ESP_OK) return (int)err;

    ESP_LOGI(TAG, "model 分区 total=%u used=%u", (unsigned)total, (unsigned)used);
    return 0;
}

/* ========================================================================
 * 第七节: 推理 Worker 任务 (Core-1)
 * ======================================================================== */

/**
 * 推理主循环:
 *   1. 检查 PSRAM ≥ 2MB + 挂载 SPIFFS
 *   2. 构建 Transformer / Tokenizer / Sampler
 *   3. 进入请求处理循环: 从队列取请求 → generate() → 更新快照
 */
static void inference_worker(void *arg)
{
    (void)arg;

    /* ---- 阶段 1: 加载模型 ---- */
    runtime_lock();
    g_inference.snapshot.state = PWOS_INFERENCE_LOADING;
    runtime_unlock();

    uint32_t psram_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total < 2u * 1024u * 1024u || mount_model_partition() != 0) {
        ESP_LOGE(TAG, "PSRAM 不足或分区挂载失败 (psram=%lu KB)",
                 (unsigned long)psram_total / 1024u);
        runtime_lock();
        g_inference.snapshot.state      = PWOS_INFERENCE_ERROR;
        g_inference.snapshot.last_error = -1;
        runtime_unlock();
        vTaskDelete(NULL);  /* 自删除，系统将因推理不可用而重启 */
        return;
    }

    build_transformer(&g_inference.transformer,
                      const_cast<char *>(PWOS_INFERENCE_MODEL_PATH));
    build_tokenizer(&g_inference.tokenizer,
                    const_cast<char *>(PWOS_INFERENCE_TOKENIZER_PATH),
                    g_inference.transformer.config.vocab_size);
    build_sampler(&g_inference.sampler,
                  g_inference.transformer.config.vocab_size,
                  0.8f, 0.9f, esp_random());

    /* ---- 阶段 2: 就绪 ---- */
    runtime_lock();
    g_inference.snapshot.state       = PWOS_INFERENCE_READY;
    g_inference.snapshot.psram_total = psram_total;
    g_inference.snapshot.psram_free  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    runtime_unlock();

    ESP_LOGI(TAG, "模型就绪 dim=%d layers=%d seq=%d vocab=%d psram_free=%lu KB",
             g_inference.transformer.config.dim,
             g_inference.transformer.config.n_layers,
             g_inference.transformer.config.seq_len,
             g_inference.transformer.config.vocab_size,
             (unsigned long)g_inference.snapshot.psram_free / 1024u);

    /* ---- 阶段 3: 请求处理循环 ---- */
    for (;;) {
        inference_request_t request;

        /* 阻塞等待请求 (无请求时让出 CPU) */
        if (xQueueReceive(g_inference.queue, &request, portMAX_DELAY) != pdTRUE)
            continue;

        /* 标记运行中 */
        runtime_lock();
        g_inference.snapshot.state = PWOS_INFERENCE_RUNNING;
        runtime_unlock();

        /* 应用请求中的采样参数 */
        g_inference.sampler.temperature = request.temperature;
        g_inference.sampler.topp        = request.topp;
        g_inference.sampler.rng_state   = esp_random();

        ESP_LOGI(TAG, "[推理] request=%lu prompt=\"%s\"",
                 (unsigned long)request.request_id, request.prompt);

        /* 执行生成 */
        generate(&g_inference.transformer,
                 &g_inference.tokenizer,
                 &g_inference.sampler,
                 request.prompt,
                 request.steps,
                 output_piece,
                 generation_done,
                 NULL);

        /* 标记完成，更新统计 */
        runtime_lock();
        g_inference.snapshot.state = PWOS_INFERENCE_DONE;
        g_inference.snapshot.runs++;
        g_inference.snapshot.psram_free =
            (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        runtime_unlock();

        ESP_LOGI(TAG, "[完成] request=%lu bytes=%u speed=%.2f tok/s",
                 (unsigned long)request.request_id,
                 g_inference.snapshot.generated_bytes,
                 (double)g_inference.snapshot.tokens_per_second);
    }
}

/* ========================================================================
 * 第八节: 控制台输入任务 (stdin)
 * ======================================================================== */

/**
 * 监控 stdin，将每一行作为 prompt 提交推理请求。
 * (仅调试用，生产环境通过 9P/RPC 提交请求)
 */
static void inference_console(void *arg)
{
    char prompt[PWOS_INFERENCE_PROMPT_CAP];

    (void)arg;
    for (;;) {
        fflush(stdout);
        if (!fgets(prompt, sizeof(prompt), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));   /* stdin 不可用，短暂休眠 */
            continue;
        }

        /* 去除尾部空白 (换行符等) */
        size_t len = strlen(prompt);
        while (len > 0u && isspace((unsigned char)prompt[len - 1u]))
            prompt[--len] = '\0';
        if (len == 0u) continue;

        /* 提交推理请求 */
        uint32_t req_id;
        if (pwos_inference_runtime_submit(prompt, 128u, 0.8f, 0.9f, &req_id) != 0) {
            ESP_LOGW(TAG, "推理忙或模型未就绪");
        }
    }
}

/* ========================================================================
 * 第九节: 公开 API 实现
 * ======================================================================== */

/**
 * 启动推理运行时 (幂等)。
 *
 * 初始化流程:
 *   mutex + queue + output 缓冲区
 *   → 创建 llm_worker (Core-1, 加载模型)
 *   → 创建 llm_console (stdin 监控)
 *
 * 若任一步失败，完整回滚已分配资源。
 */
int pwos_inference_runtime_start(void)
{
    /* 幂等: 已初始化则直接返回 */
    if (g_inference.snapshot.initialized != 0u) return 0;

    memset(&g_inference, 0, sizeof(g_inference));

    /* 分配同步对象和输出缓冲区 */
    g_inference.mutex  = xSemaphoreCreateMutex();
    g_inference.queue  = xQueueCreate(1u, sizeof(inference_request_t));
    g_inference.output = (char *)heap_caps_calloc(PWOS_INFERENCE_OUTPUT_CAP, 1u,
                                                   MALLOC_CAP_SPIRAM);
    if (!g_inference.mutex || !g_inference.queue || !g_inference.output) {
        if (g_inference.mutex)  vSemaphoreDelete(g_inference.mutex);
        if (g_inference.queue)  vQueueDelete(g_inference.queue);
        if (g_inference.output) heap_caps_free(g_inference.output);
        memset(&g_inference, 0, sizeof(g_inference));
        return -1;
    }

    g_inference.snapshot.initialized = 1u;
    g_inference.snapshot.state       = PWOS_INFERENCE_OFFLINE;

    /* 创建 worker 任务 */
    BaseType_t created = xTaskCreatePinnedToCore(
        inference_worker, "llm_worker",
        PWOS_INFERENCE_WORKER_STACK, NULL,
        5u, &g_inference.worker_task, 1);  /* Core-1 */

    if (created != pdPASS) {
        vSemaphoreDelete(g_inference.mutex);
        vQueueDelete(g_inference.queue);
        heap_caps_free(g_inference.output);
        memset(&g_inference, 0, sizeof(g_inference));
        return -1;
    }

    /* 创建 console 任务 */
    created = xTaskCreate(inference_console, "llm_console",
                          PWOS_INFERENCE_CONSOLE_STACK, NULL,
                          3u, &g_inference.console_task);

    if (created != pdPASS) {
        /* ★ 修复: console 创建失败时，删除已运行的 worker 并清理全部资源 */
        vTaskDelete(g_inference.worker_task);
        vSemaphoreDelete(g_inference.mutex);
        vQueueDelete(g_inference.queue);
        heap_caps_free(g_inference.output);
        memset(&g_inference, 0, sizeof(g_inference));
        return -1;
    }

    return 0;
}

/**
 * 提交推理请求 (非阻塞)。
 *
 * 参数校验 → 持锁检查状态 → 构造请求 → 入队 → 恢复状态 (若入队失败)。
 */
int pwos_inference_runtime_submit(
    const char *prompt, uint16_t steps,
    float temperature, float topp,
    uint32_t *out_request_id)
{
    /* ---- 参数校验 ---- */
    if (!prompt || !prompt[0] || !out_request_id ||
        strlen(prompt) >= PWOS_INFERENCE_PROMPT_CAP ||
        steps == 0u || temperature < 0.0f || topp < 0.0f || topp > 1.0f) {
        return -1;
    }

    inference_request_t request;
    memset(&request, 0, sizeof(request));

    /* ---- 状态检查 (持锁) ---- */
    runtime_lock();
    uint8_t prev_state = g_inference.snapshot.state;
    if (prev_state != PWOS_INFERENCE_READY && prev_state != PWOS_INFERENCE_DONE) {
        g_inference.snapshot.rejected_busy++;
        runtime_unlock();
        return -2;  /* 模型未就绪或正忙 */
    }

    /* ---- 构造请求 ---- */
    g_inference.snapshot.request_id++;
    if (g_inference.snapshot.request_id == 0u)
        g_inference.snapshot.request_id++;  /* 避免 ID=0 */

    request.request_id  = g_inference.snapshot.request_id;
    request.steps       = (steps > (uint16_t)g_inference.transformer.config.seq_len)
                            ? (uint16_t)g_inference.transformer.config.seq_len : steps;
    request.temperature = temperature;
    request.topp        = topp;

    /* 安全拷贝 prompt (snprintf 保证 null 终止) */
    (void)snprintf(request.prompt, sizeof(request.prompt), "%s", prompt);

    /* 重置本轮统计 */
    g_inference.snapshot.requested_steps    = request.steps;
    g_inference.snapshot.generated_bytes    = 0u;
    g_inference.snapshot.tokens_per_second  = 0.0f;
    g_inference.snapshot.state              = PWOS_INFERENCE_QUEUED;
    g_inference.output[0]                   = '\0';
    runtime_unlock();

    /* ---- 入队 (非阻塞) ---- */
    if (xQueueSend(g_inference.queue, &request, 0u) != pdTRUE) {
        /* 入队失败: 恢复先前状态 */
        runtime_lock();
        g_inference.snapshot.state = prev_state;
        g_inference.snapshot.rejected_busy++;
        runtime_unlock();
        return -2;
    }

    *out_request_id = request.request_id;
    return 0;
}

/**
 * 获取运行时快照 (线程安全)。
 * 若运行时尚未初始化 (mutex==NULL)，返回全零快照。
 */
void pwos_inference_runtime_get_snapshot(
    pwos_inference_snapshot_t *out_snapshot)
{
    if (!out_snapshot) return;

    if (!g_inference.mutex) {
        memset(out_snapshot, 0, sizeof(*out_snapshot));
        return;
    }
    runtime_lock();
    *out_snapshot = g_inference.snapshot;
    runtime_unlock();
}

/**
 * 读取推理结果 (分块，线程安全)。
 *
 * @param offset 从 0 开始；每次调用可递增 offset 实现流式读取。
 * @return 0=成功, -1=请求 ID 不匹配或 offset 越界
 */
int pwos_inference_runtime_read_result(
    uint32_t request_id, uint16_t offset,
    uint8_t *out, uint16_t *in_out_len)
{
    if (!out || !in_out_len || !g_inference.mutex) return -1;

    runtime_lock();

    /* 校验: 请求 ID 必须匹配当前请求，offset 不能超过已生成字节数 */
    if (request_id != g_inference.snapshot.request_id ||
        offset > g_inference.snapshot.generated_bytes) {
        runtime_unlock();
        return -1;
    }

    uint16_t available = g_inference.snapshot.generated_bytes - offset;
    uint16_t copy_len  = (*in_out_len < available) ? *in_out_len : available;

    memcpy(out, g_inference.output + offset, copy_len);
    *in_out_len = copy_len;

    runtime_unlock();
    return 0;
}
