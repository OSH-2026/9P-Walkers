#ifndef PWOS_JOB_SERVICE_H
#define PWOS_JOB_SERVICE_H

#include <stdint.h>

#include "compute_worker.h"
#include "pwos_job_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*pwos_job_service_send_fn)(
    void *ctx,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len);

typedef struct {
    uint32_t request_rx;
    uint32_t response_tx;
    uint32_t caps_rx;
    uint32_t submit_rx;
    uint32_t status_rx;
    uint32_t result_rx;
    uint32_t cancel_rx;
    uint32_t bad_frames;
    uint32_t send_failures;
    uint16_t last_request_id;
    uint16_t last_status;
    uint32_t last_job_id;
} pwos_job_service_stats_t;

typedef struct {
    void *io_ctx;
    pwos_job_service_send_fn send;
    pwos_compute_worker_t *worker;
    uint8_t tx_frame[PWOS_JOB_MAX_FRAME_LEN];
    uint8_t result[PWOS_COMPUTE_RESULT_CAP];
    pwos_job_service_stats_t stats;
} pwos_job_service_t;

int pwos_job_service_init(
    pwos_job_service_t *service,
    void *io_ctx,
    pwos_job_service_send_fn send,
    pwos_compute_worker_t *worker);

int pwos_job_service_process(
    pwos_job_service_t *service,
    uint8_t src_addr,
    const uint8_t *frame,
    uint16_t frame_len);

void pwos_job_service_get_stats(
    const pwos_job_service_t *service,
    pwos_job_service_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_JOB_SERVICE_H */
