#ifndef PWOS_INFERENCE_RUNTIME_H
#define PWOS_INFERENCE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_INFERENCE_PROMPT_CAP 192u
#define PWOS_INFERENCE_OUTPUT_CAP 4096u

typedef enum {
    PWOS_INFERENCE_OFFLINE = 0,
    PWOS_INFERENCE_LOADING = 1,
    PWOS_INFERENCE_READY = 2,
    PWOS_INFERENCE_QUEUED = 3,
    PWOS_INFERENCE_RUNNING = 4,
    PWOS_INFERENCE_DONE = 5,
    PWOS_INFERENCE_ERROR = 6,
} pwos_inference_state_t;

typedef struct {
    uint8_t initialized;
    uint8_t state;
    uint32_t request_id;
    uint16_t requested_steps;
    uint16_t generated_bytes;
    uint32_t runs;
    uint32_t rejected_busy;
    uint32_t psram_total;
    uint32_t psram_free;
    float tokens_per_second;
    int32_t last_error;
} pwos_inference_snapshot_t;

int pwos_inference_runtime_start(void);

int pwos_inference_runtime_submit(
    const char *prompt,
    uint16_t steps,
    float temperature,
    float topp,
    uint32_t *out_request_id);

void pwos_inference_runtime_get_snapshot(
    pwos_inference_snapshot_t *out_snapshot);

int pwos_inference_runtime_read_result(
    uint32_t request_id,
    uint16_t offset,
    uint8_t *out,
    uint16_t *in_out_len);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_INFERENCE_RUNTIME_H */
