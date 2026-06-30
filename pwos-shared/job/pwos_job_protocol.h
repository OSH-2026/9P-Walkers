#ifndef PWOS_JOB_PROTOCOL_H
#define PWOS_JOB_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_JOB_VERSION 1u
#define PWOS_JOB_HEADER_LEN 20u
#define PWOS_JOB_MAX_FRAME_LEN 512u
#define PWOS_JOB_MAX_PAYLOAD_LEN (PWOS_JOB_MAX_FRAME_LEN - PWOS_JOB_HEADER_LEN)
#define PWOS_JOB_PROGRESS_MAX 1000u

typedef enum {
    PWOS_JOB_KIND_CAPS_REQUEST = 1,
    PWOS_JOB_KIND_CAPS_RESPONSE = 2,
    PWOS_JOB_KIND_SUBMIT = 3,
    PWOS_JOB_KIND_SUBMIT_ACK = 4,
    PWOS_JOB_KIND_STATUS_REQUEST = 5,
    PWOS_JOB_KIND_STATUS_RESPONSE = 6,
    PWOS_JOB_KIND_RESULT_REQUEST = 7,
    PWOS_JOB_KIND_RESULT_RESPONSE = 8,
    PWOS_JOB_KIND_CANCEL_REQUEST = 9,
    PWOS_JOB_KIND_CANCEL_ACK = 10,
} pwos_job_kind_t;

typedef enum {
    PWOS_JOB_STATE_EMPTY = 0,
    PWOS_JOB_STATE_CREATED = 1,
    PWOS_JOB_STATE_QUEUED = 2,
    PWOS_JOB_STATE_ASSIGNED = 3,
    PWOS_JOB_STATE_RUNNING = 4,
    PWOS_JOB_STATE_DONE = 5,
    PWOS_JOB_STATE_FAILED = 6,
    PWOS_JOB_STATE_CANCELLED = 7,
    PWOS_JOB_STATE_LOST = 8,
} pwos_job_state_t;

typedef enum {
    PWOS_JOB_KERNEL_NONE = 0,
    PWOS_JOB_KERNEL_HASH = 1,
    PWOS_JOB_KERNEL_VECTOR_ADD = 2,
    PWOS_JOB_KERNEL_MATMUL = 3,
    PWOS_JOB_KERNEL_MANDELBROT = 4,
    PWOS_JOB_KERNEL_RAYTRACE_TILE = 5,
} pwos_job_kernel_t;

typedef enum {
    PWOS_JOB_STATUS_OK = 0,
    PWOS_JOB_STATUS_BAD_REQUEST = 1,
    PWOS_JOB_STATUS_NOT_FOUND = 2,
    PWOS_JOB_STATUS_BUSY = 3,
    PWOS_JOB_STATUS_UNSUPPORTED = 4,
    PWOS_JOB_STATUS_INTERNAL = 5,
    PWOS_JOB_STATUS_NOT_READY = 6,
    PWOS_JOB_STATUS_CANCELLED = 7,
} pwos_job_status_t;

typedef struct {
    uint8_t kind;
    uint8_t state;
    uint8_t kernel;
    uint16_t request_id;
    uint16_t status;
    uint32_t job_id;
    uint16_t progress_permille;
    uint32_t result_len;
    const uint8_t *payload;
    uint16_t payload_len;
} pwos_job_frame_view_t;

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
    size_t *out_len);

int pwos_job_decode(
    const uint8_t *frame,
    size_t frame_len,
    pwos_job_frame_view_t *out_view);

int pwos_job_retag(uint8_t *frame, size_t frame_len, uint16_t request_id);

uint16_t pwos_job_get_le16(const uint8_t *src);
uint32_t pwos_job_get_le32(const uint8_t *src);
int16_t pwos_job_get_i16(const uint8_t *src);
int32_t pwos_job_get_i32(const uint8_t *src);
void pwos_job_put_le16(uint8_t *dst, uint16_t value);
void pwos_job_put_le32(uint8_t *dst, uint32_t value);
void pwos_job_put_i16(uint8_t *dst, int16_t value);
void pwos_job_put_i32(uint8_t *dst, int32_t value);

const char *pwos_job_state_name(uint8_t state);
const char *pwos_job_kernel_name(uint8_t kernel);
const char *pwos_job_status_name(uint16_t status);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_JOB_PROTOCOL_H */
