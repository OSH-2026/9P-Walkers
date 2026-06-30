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

#define PWOS_INFERENCE_MODEL_PATH "/model/stories260K.bin"
#define PWOS_INFERENCE_TOKENIZER_PATH "/model/tok512.bin"
#define PWOS_INFERENCE_WORKER_STACK 16384u
#define PWOS_INFERENCE_CONSOLE_STACK 4096u

typedef struct {
    uint32_t request_id;
    uint16_t steps;
    float temperature;
    float topp;
    char prompt[PWOS_INFERENCE_PROMPT_CAP];
} inference_request_t;

typedef struct {
    Transformer transformer;
    Tokenizer tokenizer;
    Sampler sampler;
    QueueHandle_t queue;
    SemaphoreHandle_t mutex;
    TaskHandle_t worker_task;
    TaskHandle_t console_task;
    char *output;
    pwos_inference_snapshot_t snapshot;
} inference_runtime_t;

static const char *TAG = "pwos_inference";
static inference_runtime_t g_inference;

static void runtime_lock(void)
{
    (void)xSemaphoreTake(g_inference.mutex, portMAX_DELAY);
}

static void runtime_unlock(void)
{
    (void)xSemaphoreGive(g_inference.mutex);
}

static int printable_piece(const char *piece)
{
    if (piece == NULL || piece[0] == '\0') return 0;
    if (piece[1] == '\0') {
        unsigned char value = (unsigned char)piece[0];

        return isprint(value) || isspace(value);
    }
    return 1;
}

static void output_piece(void *ctx, const char *piece)
{
    size_t piece_len;
    size_t available;

    (void)ctx;
    if (!printable_piece(piece)) return;
    piece_len = strlen(piece);
    runtime_lock();
    available = PWOS_INFERENCE_OUTPUT_CAP - 1u -
        g_inference.snapshot.generated_bytes;
    if (piece_len > available) piece_len = available;
    if (piece_len > 0u) {
        memcpy(
            g_inference.output + g_inference.snapshot.generated_bytes,
            piece,
            piece_len);
        g_inference.snapshot.generated_bytes += (uint16_t)piece_len;
        g_inference.output[g_inference.snapshot.generated_bytes] = '\0';
    }
    runtime_unlock();
    printf("%s", piece);
    fflush(stdout);
}

static void generation_done(void *ctx, float tokens_per_second)
{
    (void)ctx;
    runtime_lock();
    g_inference.snapshot.tokens_per_second = tokens_per_second;
    runtime_unlock();
}

static int mount_model_partition(void)
{
    esp_vfs_spiffs_conf_t config = {
        .base_path = "/model",
        .partition_label = "model",
        .max_files = 3,
        .format_if_mount_failed = false,
    };
    size_t total = 0u;
    size_t used = 0u;
    esp_err_t error;

    error = esp_vfs_spiffs_register(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "model partition mount failed: %s", esp_err_to_name(error));
        return (int)error;
    }
    error = esp_spiffs_info("model", &total, &used);
    if (error != ESP_OK) return (int)error;
    ESP_LOGI(TAG, "model partition total=%u used=%u",
        (unsigned)total, (unsigned)used);
    return 0;
}

static void inference_worker(void *arg)
{
    inference_request_t request;
    uint32_t psram_total;

    (void)arg;
    runtime_lock();
    g_inference.snapshot.state = PWOS_INFERENCE_LOADING;
    runtime_unlock();

    psram_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total < 2u * 1024u * 1024u || mount_model_partition() != 0) {
        runtime_lock();
        g_inference.snapshot.state = PWOS_INFERENCE_ERROR;
        g_inference.snapshot.last_error = -1;
        runtime_unlock();
        vTaskDelete(NULL);
        return;
    }
    build_transformer(
        &g_inference.transformer,
        const_cast<char *>(PWOS_INFERENCE_MODEL_PATH));
    build_tokenizer(
        &g_inference.tokenizer,
        const_cast<char *>(PWOS_INFERENCE_TOKENIZER_PATH),
        g_inference.transformer.config.vocab_size);
    build_sampler(
        &g_inference.sampler,
        g_inference.transformer.config.vocab_size,
        0.8f,
        0.9f,
        esp_random());

    runtime_lock();
    g_inference.snapshot.state = PWOS_INFERENCE_READY;
    g_inference.snapshot.psram_total = psram_total;
    g_inference.snapshot.psram_free =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    runtime_unlock();
    ESP_LOGI(TAG, "model ready dim=%d layers=%d seq=%d vocab=%d psram_free=%lu",
        g_inference.transformer.config.dim,
        g_inference.transformer.config.n_layers,
        g_inference.transformer.config.seq_len,
        g_inference.transformer.config.vocab_size,
        (unsigned long)g_inference.snapshot.psram_free);

    for (;;) {
        if (xQueueReceive(g_inference.queue, &request, portMAX_DELAY) != pdTRUE) continue;
        runtime_lock();
        g_inference.snapshot.state = PWOS_INFERENCE_RUNNING;
        runtime_unlock();
        g_inference.sampler.temperature = request.temperature;
        g_inference.sampler.topp = request.topp;
        g_inference.sampler.rng_state = esp_random();
        printf("\n[inference request=%lu prompt=\"%s\"]\n",
            (unsigned long)request.request_id, request.prompt);
        generate(
            &g_inference.transformer,
            &g_inference.tokenizer,
            &g_inference.sampler,
            request.prompt,
            request.steps,
            output_piece,
            generation_done,
            NULL);
        runtime_lock();
        g_inference.snapshot.state = PWOS_INFERENCE_DONE;
        ++g_inference.snapshot.runs;
        g_inference.snapshot.psram_free =
            (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        runtime_unlock();
        ESP_LOGI(TAG, "request=%lu done bytes=%u speed=%.2f tok/s",
            (unsigned long)request.request_id,
            g_inference.snapshot.generated_bytes,
            (double)g_inference.snapshot.tokens_per_second);
    }
}

static void inference_console(void *arg)
{
    char prompt[PWOS_INFERENCE_PROMPT_CAP];

    (void)arg;
    for (;;) {
        size_t len;
        uint32_t request_id;

        fflush(stdout);
        if (fgets(prompt, sizeof(prompt), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        len = strlen(prompt);
        while (len > 0u && isspace((unsigned char)prompt[len - 1u])) {
            prompt[--len] = '\0';
        }
        if (len == 0u) continue;
        if (pwos_inference_runtime_submit(
                prompt, 128u, 0.8f, 0.9f, &request_id) != 0) {
            ESP_LOGW(TAG, "inference busy or model not ready");
        }
    }
}

int pwos_inference_runtime_start(void)
{
    BaseType_t created;

    if (g_inference.snapshot.initialized != 0u) return 0;
    memset(&g_inference, 0, sizeof(g_inference));
    g_inference.mutex = xSemaphoreCreateMutex();
    g_inference.queue = xQueueCreate(1u, sizeof(inference_request_t));
    g_inference.output = static_cast<char *>(heap_caps_calloc(
        PWOS_INFERENCE_OUTPUT_CAP, 1u, MALLOC_CAP_SPIRAM));
    if (g_inference.mutex == NULL || g_inference.queue == NULL ||
        g_inference.output == NULL) return -1;
    g_inference.snapshot.initialized = 1u;
    g_inference.snapshot.state = PWOS_INFERENCE_OFFLINE;
    created = xTaskCreatePinnedToCore(
        inference_worker,
        "llm_worker",
        PWOS_INFERENCE_WORKER_STACK,
        NULL,
        5u,
        &g_inference.worker_task,
        1);
    if (created != pdPASS) return -1;
    created = xTaskCreate(
        inference_console,
        "llm_console",
        PWOS_INFERENCE_CONSOLE_STACK,
        NULL,
        3u,
        &g_inference.console_task);
    return created == pdPASS ? 0 : -1;
}

int pwos_inference_runtime_submit(
    const char *prompt,
    uint16_t steps,
    float temperature,
    float topp,
    uint32_t *out_request_id)
{
    inference_request_t request;
    uint8_t state;

    if (prompt == NULL || prompt[0] == '\0' || out_request_id == NULL ||
        strlen(prompt) >= sizeof(request.prompt) || steps == 0u ||
        temperature < 0.0f || topp < 0.0f || topp > 1.0f) return -1;
    memset(&request, 0, sizeof(request));
    runtime_lock();
    state = g_inference.snapshot.state;
    if (state != PWOS_INFERENCE_READY && state != PWOS_INFERENCE_DONE) {
        ++g_inference.snapshot.rejected_busy;
        runtime_unlock();
        return -2;
    }
    ++g_inference.snapshot.request_id;
    if (g_inference.snapshot.request_id == 0u) ++g_inference.snapshot.request_id;
    request.request_id = g_inference.snapshot.request_id;
    request.steps = steps > (uint16_t)g_inference.transformer.config.seq_len ?
        (uint16_t)g_inference.transformer.config.seq_len : steps;
    request.temperature = temperature;
    request.topp = topp;
    (void)snprintf(request.prompt, sizeof(request.prompt), "%s", prompt);
    g_inference.snapshot.requested_steps = request.steps;
    g_inference.snapshot.generated_bytes = 0u;
    g_inference.snapshot.tokens_per_second = 0.0f;
    g_inference.snapshot.state = PWOS_INFERENCE_QUEUED;
    g_inference.output[0] = '\0';
    runtime_unlock();
    if (xQueueSend(g_inference.queue, &request, 0u) != pdTRUE) {
        runtime_lock();
        g_inference.snapshot.state = state;
        ++g_inference.snapshot.rejected_busy;
        runtime_unlock();
        return -2;
    }
    *out_request_id = request.request_id;
    return 0;
}

void pwos_inference_runtime_get_snapshot(
    pwos_inference_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) return;
    if (g_inference.mutex == NULL) {
        memset(out_snapshot, 0, sizeof(*out_snapshot));
        return;
    }
    runtime_lock();
    *out_snapshot = g_inference.snapshot;
    runtime_unlock();
}

int pwos_inference_runtime_read_result(
    uint32_t request_id,
    uint16_t offset,
    uint8_t *out,
    uint16_t *in_out_len)
{
    uint16_t available;
    uint16_t copy_len;

    if (out == NULL || in_out_len == NULL || g_inference.mutex == NULL) return -1;
    runtime_lock();
    if (request_id != g_inference.snapshot.request_id ||
        offset > g_inference.snapshot.generated_bytes) {
        runtime_unlock();
        return -1;
    }
    available = g_inference.snapshot.generated_bytes - offset;
    copy_len = *in_out_len < available ? *in_out_len : available;
    memcpy(out, g_inference.output + offset, copy_len);
    *in_out_len = copy_len;
    runtime_unlock();
    return 0;
}
