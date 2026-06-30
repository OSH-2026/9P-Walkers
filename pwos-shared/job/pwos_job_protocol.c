#include "pwos_job_protocol.h"

#include <string.h>

enum {
    JOB_OFF_VERSION = 0,
    JOB_OFF_KIND = 1,
    JOB_OFF_STATE = 2,
    JOB_OFF_KERNEL = 3,
    JOB_OFF_REQUEST_ID = 4,
    JOB_OFF_STATUS = 6,
    JOB_OFF_JOB_ID = 8,
    JOB_OFF_PROGRESS = 12,
    JOB_OFF_PAYLOAD_LEN = 14,
    JOB_OFF_RESULT_LEN = 16,
};

uint16_t pwos_job_get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

uint32_t pwos_job_get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

int16_t pwos_job_get_i16(const uint8_t *src)
{
    return (int16_t)pwos_job_get_le16(src);
}

int32_t pwos_job_get_i32(const uint8_t *src)
{
    return (int32_t)pwos_job_get_le32(src);
}

void pwos_job_put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)(value >> 8);
}

void pwos_job_put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)(value >> 24);
}

void pwos_job_put_i16(uint8_t *dst, int16_t value)
{
    pwos_job_put_le16(dst, (uint16_t)value);
}

void pwos_job_put_i32(uint8_t *dst, int32_t value)
{
    pwos_job_put_le32(dst, (uint32_t)value);
}

static int kind_valid(uint8_t kind)
{
    return kind >= PWOS_JOB_KIND_CAPS_REQUEST &&
        kind <= PWOS_JOB_KIND_CANCEL_ACK;
}

static int fields_valid(uint8_t state, uint8_t kernel, uint16_t status)
{
    return state <= PWOS_JOB_STATE_LOST &&
        kernel <= PWOS_JOB_KERNEL_MANDELBROT &&
        status <= PWOS_JOB_STATUS_CANCELLED;
}

int pwos_job_encode(
    uint8_t kind,
    uint8_t state,
    uint8_t kernel,
    uint16_t request_id,
    uint16_t status,
    uint32_t job_id,
    uint16_t progress_permille,
    uint32_t result_len,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len)
{
    size_t total = PWOS_JOB_HEADER_LEN + payload_len;

    if (out == NULL || out_len == NULL || !kind_valid(kind) ||
        !fields_valid(state, kernel, status) ||
        progress_permille > PWOS_JOB_PROGRESS_MAX ||
        payload_len > PWOS_JOB_MAX_PAYLOAD_LEN ||
        (payload_len > 0u && payload == NULL) || total > out_cap) {
        return -1;
    }

    memset(out, 0, PWOS_JOB_HEADER_LEN);
    out[JOB_OFF_VERSION] = PWOS_JOB_VERSION;
    out[JOB_OFF_KIND] = kind;
    out[JOB_OFF_STATE] = state;
    out[JOB_OFF_KERNEL] = kernel;
    pwos_job_put_le16(out + JOB_OFF_REQUEST_ID, request_id);
    pwos_job_put_le16(out + JOB_OFF_STATUS, status);
    pwos_job_put_le32(out + JOB_OFF_JOB_ID, job_id);
    pwos_job_put_le16(out + JOB_OFF_PROGRESS, progress_permille);
    pwos_job_put_le16(out + JOB_OFF_PAYLOAD_LEN, payload_len);
    pwos_job_put_le32(out + JOB_OFF_RESULT_LEN, result_len);
    if (payload_len > 0u) {
        memcpy(out + PWOS_JOB_HEADER_LEN, payload, payload_len);
    }
    *out_len = total;
    return 0;
}

int pwos_job_decode(
    const uint8_t *frame,
    size_t frame_len,
    pwos_job_frame_view_t *out_view)
{
    uint16_t payload_len;

    if (frame == NULL || out_view == NULL || frame_len < PWOS_JOB_HEADER_LEN ||
        frame_len > PWOS_JOB_MAX_FRAME_LEN ||
        frame[JOB_OFF_VERSION] != PWOS_JOB_VERSION ||
        !kind_valid(frame[JOB_OFF_KIND]) ||
        !fields_valid(
            frame[JOB_OFF_STATE],
            frame[JOB_OFF_KERNEL],
            pwos_job_get_le16(frame + JOB_OFF_STATUS))) {
        return -1;
    }
    payload_len = pwos_job_get_le16(frame + JOB_OFF_PAYLOAD_LEN);
    if ((size_t)PWOS_JOB_HEADER_LEN + payload_len != frame_len ||
        pwos_job_get_le16(frame + JOB_OFF_PROGRESS) > PWOS_JOB_PROGRESS_MAX) {
        return -1;
    }

    memset(out_view, 0, sizeof(*out_view));
    out_view->kind = frame[JOB_OFF_KIND];
    out_view->state = frame[JOB_OFF_STATE];
    out_view->kernel = frame[JOB_OFF_KERNEL];
    out_view->request_id = pwos_job_get_le16(frame + JOB_OFF_REQUEST_ID);
    out_view->status = pwos_job_get_le16(frame + JOB_OFF_STATUS);
    out_view->job_id = pwos_job_get_le32(frame + JOB_OFF_JOB_ID);
    out_view->progress_permille = pwos_job_get_le16(frame + JOB_OFF_PROGRESS);
    out_view->payload_len = payload_len;
    out_view->result_len = pwos_job_get_le32(frame + JOB_OFF_RESULT_LEN);
    out_view->payload = frame + PWOS_JOB_HEADER_LEN;
    return 0;
}

int pwos_job_retag(uint8_t *frame, size_t frame_len, uint16_t request_id)
{
    pwos_job_frame_view_t view;

    if (pwos_job_decode(frame, frame_len, &view) != 0) {
        return -1;
    }
    pwos_job_put_le16(frame + JOB_OFF_REQUEST_ID, request_id);
    return 0;
}

const char *pwos_job_state_name(uint8_t state)
{
    switch (state) {
    case PWOS_JOB_STATE_EMPTY: return "empty";
    case PWOS_JOB_STATE_CREATED: return "created";
    case PWOS_JOB_STATE_QUEUED: return "queued";
    case PWOS_JOB_STATE_ASSIGNED: return "assigned";
    case PWOS_JOB_STATE_RUNNING: return "running";
    case PWOS_JOB_STATE_DONE: return "done";
    case PWOS_JOB_STATE_FAILED: return "failed";
    case PWOS_JOB_STATE_CANCELLED: return "cancelled";
    case PWOS_JOB_STATE_LOST: return "lost";
    default: return "unknown";
    }
}

const char *pwos_job_kernel_name(uint8_t kernel)
{
    switch (kernel) {
    case PWOS_JOB_KERNEL_HASH: return "hash";
    case PWOS_JOB_KERNEL_VECTOR_ADD: return "vector_add";
    case PWOS_JOB_KERNEL_MATMUL: return "matmul";
    case PWOS_JOB_KERNEL_MANDELBROT: return "mandelbrot";
    default: return "none";
    }
}

const char *pwos_job_status_name(uint16_t status)
{
    switch (status) {
    case PWOS_JOB_STATUS_OK: return "ok";
    case PWOS_JOB_STATUS_BAD_REQUEST: return "bad_request";
    case PWOS_JOB_STATUS_NOT_FOUND: return "not_found";
    case PWOS_JOB_STATUS_BUSY: return "busy";
    case PWOS_JOB_STATUS_UNSUPPORTED: return "unsupported";
    case PWOS_JOB_STATUS_INTERNAL: return "internal";
    case PWOS_JOB_STATUS_NOT_READY: return "not_ready";
    case PWOS_JOB_STATUS_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}
