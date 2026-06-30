#include "job_service.h"

#include <stdio.h>
#include <string.h>

static int send_response(
    pwos_job_service_t *service,
    uint8_t dst_addr,
    uint8_t kind,
    uint16_t request_id,
    uint16_t status,
    const pwos_compute_job_snapshot_t *snapshot,
    const uint8_t *payload,
    uint16_t payload_len)
{
    size_t frame_len = 0u;
    uint8_t state = snapshot == NULL ? PWOS_JOB_STATE_EMPTY : snapshot->state;
    uint8_t kernel = snapshot == NULL ? PWOS_JOB_KERNEL_NONE : snapshot->kernel;
    uint32_t job_id = snapshot == NULL ? 0u : snapshot->job_id;
    uint16_t progress = snapshot == NULL ? 0u : snapshot->progress_permille;
    uint32_t result_len = snapshot == NULL ? 0u : snapshot->result_len;
    int rc;

    if (pwos_job_encode(
            kind,
            state,
            kernel,
            request_id,
            status,
            job_id,
            progress,
            result_len,
            payload,
            payload_len,
            service->tx_frame,
            sizeof(service->tx_frame),
            &frame_len) != 0) {
        ++service->stats.send_failures;
        return -1;
    }
    rc = service->send(
        service->io_ctx, dst_addr, service->tx_frame, (uint16_t)frame_len);
    if (rc != 0) {
        ++service->stats.send_failures;
        return rc;
    }
    ++service->stats.response_tx;
    service->stats.last_request_id = request_id;
    service->stats.last_status = status;
    service->stats.last_job_id = job_id;
    return 0;
}

static int process_caps(
    pwos_job_service_t *service,
    uint8_t src_addr,
    const pwos_job_frame_view_t *request)
{
    char caps[160];
    pwos_compute_worker_stats_t stats;
    int len;

    pwos_compute_worker_get_stats(service->worker, &stats);
    len = snprintf(
        caps,
        sizeof(caps),
        "cpu=stm32f407 slots=%u input=%u result=%u active=%u queued=%u kernels=hash,vector_add,matmul,mandelbrot",
        PWOS_COMPUTE_MAX_JOBS,
        PWOS_COMPUTE_INPUT_CAP,
        PWOS_COMPUTE_RESULT_CAP,
        stats.active_jobs,
        stats.queued_jobs);
    if (len < 0 || (size_t)len >= sizeof(caps)) {
        return -1;
    }
    ++service->stats.caps_rx;
    return send_response(
        service,
        src_addr,
        PWOS_JOB_KIND_CAPS_RESPONSE,
        request->request_id,
        PWOS_JOB_STATUS_OK,
        NULL,
        (const uint8_t *)caps,
        (uint16_t)len);
}

static int process_submit(
    pwos_job_service_t *service,
    uint8_t src_addr,
    const pwos_job_frame_view_t *request)
{
    pwos_compute_job_snapshot_t snapshot;
    uint32_t job_id = 0u;
    uint16_t status;

    ++service->stats.submit_rx;
    status = pwos_compute_worker_submit(
        service->worker,
        src_addr,
        request->kernel,
        request->payload,
        request->payload_len,
        &job_id);
    memset(&snapshot, 0, sizeof(snapshot));
    if (status == PWOS_JOB_STATUS_OK) {
        status = pwos_compute_worker_get_snapshot(
            service->worker, src_addr, job_id, &snapshot);
    }
    return send_response(
        service,
        src_addr,
        PWOS_JOB_KIND_SUBMIT_ACK,
        request->request_id,
        status,
        status == PWOS_JOB_STATUS_OK ? &snapshot : NULL,
        NULL,
        0u);
}

static int process_status(
    pwos_job_service_t *service,
    uint8_t src_addr,
    const pwos_job_frame_view_t *request)
{
    pwos_compute_job_snapshot_t snapshot;
    uint16_t status;

    ++service->stats.status_rx;
    memset(&snapshot, 0, sizeof(snapshot));
    status = pwos_compute_worker_get_snapshot(
        service->worker, src_addr, request->job_id, &snapshot);
    return send_response(
        service,
        src_addr,
        PWOS_JOB_KIND_STATUS_RESPONSE,
        request->request_id,
        status,
        status == PWOS_JOB_STATUS_OK ? &snapshot : NULL,
        NULL,
        0u);
}

static int process_result(
    pwos_job_service_t *service,
    uint8_t src_addr,
    const pwos_job_frame_view_t *request)
{
    pwos_compute_job_snapshot_t snapshot;
    uint16_t result_len = 0u;
    uint16_t status;

    ++service->stats.result_rx;
    memset(&snapshot, 0, sizeof(snapshot));
    status = pwos_compute_worker_get_result(
        service->worker,
        src_addr,
        request->job_id,
        service->result,
        sizeof(service->result),
        &result_len,
        &snapshot);
    return send_response(
        service,
        src_addr,
        PWOS_JOB_KIND_RESULT_RESPONSE,
        request->request_id,
        status,
        snapshot.job_id != 0u ? &snapshot : NULL,
        status == PWOS_JOB_STATUS_OK ? service->result : NULL,
        status == PWOS_JOB_STATUS_OK ? result_len : 0u);
}

static int process_cancel(
    pwos_job_service_t *service,
    uint8_t src_addr,
    const pwos_job_frame_view_t *request)
{
    pwos_compute_job_snapshot_t snapshot;
    uint16_t status;

    ++service->stats.cancel_rx;
    memset(&snapshot, 0, sizeof(snapshot));
    status = pwos_compute_worker_cancel(
        service->worker, src_addr, request->job_id, &snapshot);
    return send_response(
        service,
        src_addr,
        PWOS_JOB_KIND_CANCEL_ACK,
        request->request_id,
        status,
        status == PWOS_JOB_STATUS_OK ? &snapshot : NULL,
        NULL,
        0u);
}

int pwos_job_service_init(
    pwos_job_service_t *service,
    void *io_ctx,
    pwos_job_service_send_fn send,
    pwos_compute_worker_t *worker)
{
    if (service == NULL || send == NULL || worker == NULL) {
        return -1;
    }
    memset(service, 0, sizeof(*service));
    service->io_ctx = io_ctx;
    service->send = send;
    service->worker = worker;
    return 0;
}

int pwos_job_service_process(
    pwos_job_service_t *service,
    uint8_t src_addr,
    const uint8_t *frame,
    uint16_t frame_len)
{
    pwos_job_frame_view_t request;

    if (service == NULL || pwos_job_decode(frame, frame_len, &request) != 0 ||
        request.request_id == 0u) {
        if (service != NULL) {
            ++service->stats.bad_frames;
        }
        return -1;
    }
    ++service->stats.request_rx;
    switch (request.kind) {
    case PWOS_JOB_KIND_CAPS_REQUEST:
        return process_caps(service, src_addr, &request);
    case PWOS_JOB_KIND_SUBMIT:
        return process_submit(service, src_addr, &request);
    case PWOS_JOB_KIND_STATUS_REQUEST:
        return process_status(service, src_addr, &request);
    case PWOS_JOB_KIND_RESULT_REQUEST:
        return process_result(service, src_addr, &request);
    case PWOS_JOB_KIND_CANCEL_REQUEST:
        return process_cancel(service, src_addr, &request);
    default:
        ++service->stats.bad_frames;
        return -1;
    }
}

void pwos_job_service_get_stats(
    const pwos_job_service_t *service,
    pwos_job_service_stats_t *out_stats)
{
    if (service == NULL || out_stats == NULL) {
        return;
    }
    *out_stats = service->stats;
}
