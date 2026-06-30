#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "job_service.h"

typedef struct {
    uint32_t sends;
    uint8_t dst;
    uint8_t frame[PWOS_JOB_MAX_FRAME_LEN];
    uint16_t frame_len;
} fake_io_t;

static int fake_send(
    void *ctx,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len)
{
    fake_io_t *io = (fake_io_t *)ctx;

    assert(payload_len <= sizeof(io->frame));
    io->dst = dst_addr;
    io->frame_len = payload_len;
    memcpy(io->frame, payload, payload_len);
    ++io->sends;
    return 0;
}

static size_t build_request(
    uint8_t kind,
    uint8_t kernel,
    uint16_t request_id,
    uint32_t job_id,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *frame)
{
    size_t frame_len = 0u;

    assert(pwos_job_encode(
        kind, PWOS_JOB_STATE_EMPTY, kernel, request_id,
        PWOS_JOB_STATUS_OK, job_id, 0u, 0u,
        payload, payload_len, frame, PWOS_JOB_MAX_FRAME_LEN, &frame_len) == 0);
    return frame_len;
}

static pwos_job_frame_view_t decode_response(const fake_io_t *io)
{
    pwos_job_frame_view_t response;

    assert(pwos_job_decode(io->frame, io->frame_len, &response) == 0);
    return response;
}

static void test_submit_status_result(void)
{
    pwos_compute_worker_t worker;
    pwos_job_service_t service;
    fake_io_t io;
    uint8_t input[20] = {2u, 2u, 2u, 0u};
    uint8_t frame[PWOS_JOB_MAX_FRAME_LEN];
    pwos_job_frame_view_t response;
    size_t frame_len;
    uint32_t job_id;
    size_t i;

    memset(&io, 0, sizeof(io));
    assert(pwos_compute_worker_init(&worker, NULL) == 0);
    assert(pwos_job_service_init(&service, &io, fake_send, &worker) == 0);
    for (i = 0u; i < 8u; ++i) {
        pwos_job_put_i16(input + 4u + i * 2u, (int16_t)(i + 1u));
    }
    frame_len = build_request(
        PWOS_JOB_KIND_SUBMIT, PWOS_JOB_KERNEL_MATMUL, 11u, 0u,
        input, sizeof(input), frame);
    assert(pwos_job_service_process(&service, 0u, frame, frame_len) == 0);
    response = decode_response(&io);
    assert(response.kind == PWOS_JOB_KIND_SUBMIT_ACK);
    assert(response.status == PWOS_JOB_STATUS_OK);
    assert(response.state == PWOS_JOB_STATE_QUEUED);
    job_id = response.job_id;

    for (i = 0u; i < 4u; ++i) {
        assert(pwos_compute_worker_step(&worker) == 1);
    }
    frame_len = build_request(
        PWOS_JOB_KIND_RESULT_REQUEST, PWOS_JOB_KERNEL_NONE, 12u, job_id,
        NULL, 0u, frame);
    assert(pwos_job_service_process(&service, 0u, frame, frame_len) == 0);
    response = decode_response(&io);
    assert(response.kind == PWOS_JOB_KIND_RESULT_RESPONSE);
    assert(response.status == PWOS_JOB_STATUS_OK);
    assert(response.state == PWOS_JOB_STATE_DONE);
    assert(response.payload_len == 20u);
    assert(pwos_job_get_i32(response.payload + 4u) == 19);
    assert(pwos_job_get_i32(response.payload + 8u) == 22);
    assert(pwos_job_get_i32(response.payload + 12u) == 43);
    assert(pwos_job_get_i32(response.payload + 16u) == 50);
}

static void test_caps_and_cancel(void)
{
    pwos_compute_worker_t worker;
    pwos_job_service_t service;
    fake_io_t io;
    uint8_t input[20] = {16u, 16u, 64u, 0u};
    uint8_t frame[PWOS_JOB_MAX_FRAME_LEN];
    char caps[PWOS_JOB_MAX_PAYLOAD_LEN + 1u];
    pwos_job_frame_view_t response;
    size_t frame_len;
    uint32_t job_id;

    memset(&io, 0, sizeof(io));
    assert(pwos_compute_worker_init(&worker, NULL) == 0);
    assert(pwos_job_service_init(&service, &io, fake_send, &worker) == 0);
    frame_len = build_request(
        PWOS_JOB_KIND_CAPS_REQUEST, PWOS_JOB_KERNEL_NONE, 20u, 0u,
        NULL, 0u, frame);
    assert(pwos_job_service_process(&service, 0u, frame, frame_len) == 0);
    response = decode_response(&io);
    assert(response.status == PWOS_JOB_STATUS_OK);
    memcpy(caps, response.payload, response.payload_len);
    caps[response.payload_len] = '\0';
    assert(strstr(caps, "mandelbrot") != NULL);

    pwos_job_put_i32(input + 4u, -2 * 65536);
    pwos_job_put_i32(input + 8u, -65536);
    pwos_job_put_i32(input + 12u, (3 * 65536) / 15);
    pwos_job_put_i32(input + 16u, (2 * 65536) / 15);
    frame_len = build_request(
        PWOS_JOB_KIND_SUBMIT, PWOS_JOB_KERNEL_MANDELBROT, 21u, 0u,
        input, sizeof(input), frame);
    assert(pwos_job_service_process(&service, 0u, frame, frame_len) == 0);
    response = decode_response(&io);
    job_id = response.job_id;
    (void)pwos_compute_worker_step(&worker);
    frame_len = build_request(
        PWOS_JOB_KIND_CANCEL_REQUEST, PWOS_JOB_KERNEL_NONE, 22u, job_id,
        NULL, 0u, frame);
    assert(pwos_job_service_process(&service, 0u, frame, frame_len) == 0);
    response = decode_response(&io);
    assert(response.status == PWOS_JOB_STATUS_OK);
    assert(response.state == PWOS_JOB_STATE_CANCELLED);
}

int main(void)
{
    test_submit_status_result();
    test_caps_and_cancel();
    puts("pwos job service tests passed");
    return 0;
}
