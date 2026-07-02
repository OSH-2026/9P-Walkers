#include "dist_inference_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "inference_runtime.h"

static const char *TAG = "pwos_dist_llm";

/* /llm/prompt  —— 写入 prompt 文本即触发推理 */
#define PWOS_LLM_PROMPT_PATH "/llm/prompt"
/* /llm/result —— 分块读取已生成的文本 */
#define PWOS_LLM_RESULT_PATH "/llm/result"
/* /llm/status —— 读取推理状态 JSON */
#define PWOS_LLM_STATUS_PATH "/llm/status"

int pwos_dist_inference_service_init(void)
{
    ESP_LOGI(TAG, "分布式推理服务已初始化");
    return 0;
}

int pwos_dist_inference_service_read(
    const char *path,
    uint8_t *out,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    pwos_inference_snapshot_t snap;
    uint16_t avail;
    uint16_t copy_len;
    int rc;

    (void)deadline_ms;

    if (path == NULL || out == NULL || in_out_len == NULL) {
        return -1;
    }

    if (strcmp(path, PWOS_LLM_STATUS_PATH) == 0) {
        /* 返回推理状态 JSON */
        uint16_t buf_cap = *in_out_len;
        uint16_t written = 0u;
        int ret;

        pwos_inference_runtime_get_snapshot(&snap);
        ret = snprintf((char *)out, buf_cap,
            "{\"state\":%d,\"request_id\":%lu,\"runs\":%lu,",
            snap.state,
            (unsigned long)snap.request_id,
            (unsigned long)snap.runs);
        if (ret < 0 || (uint16_t)ret >= buf_cap) {
            *in_out_len = 0;
            return -1;
        }
        written = (uint16_t)ret;

        /* 继续追加剩余字段 */
        ret = snprintf((char *)out + written, buf_cap - written,
            "\"generated\":%u,\"tok_s\":%.2f,\"psram_free\":%lu}",
            snap.generated_bytes,
            (double)snap.tokens_per_second,
            (unsigned long)snap.psram_free);
        if (ret < 0) {
            *in_out_len = 0;
            return -1;
        }
        written += (uint16_t)ret;
        if (written >= buf_cap) {
            /* 输出被截断，但前半段 JSON 仍可部分使用；
             * 为简化错误处理，直接返回 -1。 */
            *in_out_len = 0;
            return -1;
        }
        *in_out_len = written;
        return 0;
    }

    if (strcmp(path, PWOS_LLM_RESULT_PATH) == 0) {
        /* 分块读取推理结果，offset 用 *in_out_len 的高位传递太麻烦，
           改为每次返回当前已生成的全部文本的剩余部分。*/
        pwos_inference_runtime_get_snapshot(&snap);
        if (snap.state != PWOS_INFERENCE_DONE &&
            snap.state != PWOS_INFERENCE_RUNNING &&
            snap.state != PWOS_INFERENCE_READY) {
            *in_out_len = 0;
            return -1;
        }
        /* 尝试读取全部结果 */
        copy_len = *in_out_len;
        rc = pwos_inference_runtime_read_result(
            snap.request_id, 0u, out, &copy_len);
        if (rc != 0) {
            *in_out_len = 0;
            return rc;
        }
        *in_out_len = copy_len;
        return 0;
    }

    /* 不匹配本服务的路径 */
    return -1;
}

int pwos_dist_inference_service_write(
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    uint32_t request_id;
    int rc;

    (void)deadline_ms;

    if (path == NULL || data == NULL || out_written == NULL) {
        return -1;
    }

    if (strcmp(path, PWOS_LLM_PROMPT_PATH) != 0) {
        /* 不匹配本服务的路径 */
        return -1;
    }

    /* CBOR 解码返回的 data 没有 null 终止符，必须先拷贝到带终止符的缓冲区，
     * 否则 snprintf("%s", prompt) 会越过实际 prompt 长度读到垃圾字节。 */
    {
        char prompt_buf[PWOS_INFERENCE_PROMPT_CAP];
        uint16_t copy_len = data_len < (uint16_t)(sizeof(prompt_buf) - 1u) ?
            data_len : (uint16_t)(sizeof(prompt_buf) - 1u);
        memcpy(prompt_buf, data, copy_len);
        prompt_buf[copy_len] = '\0';
        rc = pwos_inference_runtime_submit(
            prompt_buf, 128u, 0.8f, 0.9f, &request_id);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "推理提交失败 rc=%d（模型未就绪或正忙）", rc);
        *out_written = 0;
        return rc;
    }
    ESP_LOGI(TAG, "推理已提交 request_id=%lu prompt=\"%.*s\"",
        (unsigned long)request_id,
        (int)(data_len > 60 ? 60 : data_len),
        (const char *)data);
    *out_written = data_len;
    return 0;
}

