#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pwos_job_protocol.h"

static void test_submit_round_trip(void)
{
    static const uint8_t input[] = {2u, 2u, 2u, 0u, 1u, 0u, 2u, 0u};
    uint8_t frame[PWOS_JOB_MAX_FRAME_LEN];
    pwos_job_frame_view_t view;
    size_t frame_len = 0u;

    assert(pwos_job_encode(
        PWOS_JOB_KIND_SUBMIT,
        PWOS_JOB_STATE_CREATED,
        PWOS_JOB_KERNEL_MATMUL,
        7u,
        PWOS_JOB_STATUS_OK,
        0u,
        0u,
        0u,
        input,
        sizeof(input),
        frame,
        sizeof(frame),
        &frame_len) == 0);
    assert(pwos_job_decode(frame, frame_len, &view) == 0);
    assert(view.kind == PWOS_JOB_KIND_SUBMIT);
    assert(view.kernel == PWOS_JOB_KERNEL_MATMUL);
    assert(view.request_id == 7u);
    assert(view.payload_len == sizeof(input));
    assert(memcmp(view.payload, input, sizeof(input)) == 0);
    assert(pwos_job_retag(frame, frame_len, 99u) == 0);
    assert(pwos_job_decode(frame, frame_len, &view) == 0);
    assert(view.request_id == 99u);
}

static void test_status_and_endian_helpers(void)
{
    uint8_t bytes[4];
    uint8_t frame[PWOS_JOB_HEADER_LEN];
    pwos_job_frame_view_t view;
    size_t frame_len = 0u;

    pwos_job_put_i32(bytes, -123456);
    assert(pwos_job_get_i32(bytes) == -123456);
    pwos_job_put_i16(bytes, -3210);
    assert(pwos_job_get_i16(bytes) == -3210);

    assert(pwos_job_encode(
        PWOS_JOB_KIND_STATUS_RESPONSE,
        PWOS_JOB_STATE_RUNNING,
        PWOS_JOB_KERNEL_MANDELBROT,
        3u,
        PWOS_JOB_STATUS_OK,
        42u,
        625u,
        68u,
        NULL,
        0u,
        frame,
        sizeof(frame),
        &frame_len) == 0);
    assert(pwos_job_decode(frame, frame_len, &view) == 0);
    assert(view.job_id == 42u);
    assert(view.progress_permille == 625u);
    assert(view.result_len == 68u);
}

static void test_rejects_bad_frames(void)
{
    uint8_t frame[PWOS_JOB_HEADER_LEN];
    pwos_job_frame_view_t view;
    size_t frame_len = 0u;

    assert(pwos_job_encode(
        PWOS_JOB_KIND_CAPS_REQUEST, PWOS_JOB_STATE_EMPTY,
        PWOS_JOB_KERNEL_NONE, 1u, 0u, 0u, 1001u, 0u,
        NULL, 0u, frame, sizeof(frame), &frame_len) != 0);
    assert(pwos_job_encode(
        PWOS_JOB_KIND_CAPS_REQUEST, 0xFFu,
        PWOS_JOB_KERNEL_NONE, 1u, 0u, 0u, 0u, 0u,
        NULL, 0u, frame, sizeof(frame), &frame_len) != 0);
    assert(pwos_job_encode(
        PWOS_JOB_KIND_CAPS_REQUEST, PWOS_JOB_STATE_EMPTY,
        PWOS_JOB_KERNEL_NONE, 1u, 0u, 0u, 0u, 0u,
        NULL, 0u, frame, sizeof(frame), &frame_len) == 0);
    frame[0] = 0xFFu;
    assert(pwos_job_decode(frame, frame_len, &view) != 0);
    frame[0] = PWOS_JOB_VERSION;
    frame[2] = 0xFFu;
    assert(pwos_job_decode(frame, frame_len, &view) != 0);
    frame[2] = PWOS_JOB_STATE_EMPTY;
    frame[3] = 0xFFu;
    assert(pwos_job_decode(frame, frame_len, &view) != 0);
    frame[3] = PWOS_JOB_KERNEL_NONE;
    frame[6] = 0xFFu;
    frame[7] = 0xFFu;
    assert(pwos_job_decode(frame, frame_len, &view) != 0);
}

int main(void)
{
    test_submit_round_trip();
    test_status_and_endian_helpers();
    test_rejects_bad_frames();
    puts("pwos job protocol tests passed");
    return 0;
}
