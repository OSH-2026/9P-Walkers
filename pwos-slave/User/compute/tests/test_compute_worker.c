#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "compute_worker.h"

static void run_until_terminal(
    pwos_compute_worker_t *worker,
    uint32_t job_id,
    pwos_compute_job_snapshot_t *out_snapshot)
{
    uint32_t guard;

    for (guard = 0u; guard < 2000u; ++guard) {
        assert(pwos_compute_worker_get_snapshot(
            worker, 0u, job_id, out_snapshot) == PWOS_JOB_STATUS_OK);
        if (out_snapshot->state == PWOS_JOB_STATE_DONE ||
            out_snapshot->state == PWOS_JOB_STATE_FAILED ||
            out_snapshot->state == PWOS_JOB_STATE_CANCELLED) {
            return;
        }
        (void)pwos_compute_worker_step(worker);
    }
    assert(!"job did not finish");
}

static void test_matmul(void)
{
    pwos_compute_worker_t worker;
    pwos_compute_job_snapshot_t snapshot;
    uint8_t input[20] = {2u, 2u, 2u, 0u};
    const int16_t a[] = {1, 2, 3, 4};
    const int16_t b[] = {5, 6, 7, 8};
    const int32_t expected[] = {19, 22, 43, 50};
    uint8_t result[64];
    uint16_t result_len = 0u;
    uint32_t job_id;
    size_t i;

    assert(pwos_compute_worker_init(&worker, NULL) == 0);
    for (i = 0u; i < 4u; ++i) {
        pwos_job_put_i16(input + 4u + i * 2u, a[i]);
        pwos_job_put_i16(input + 12u + i * 2u, b[i]);
    }
    assert(pwos_compute_worker_submit(
        &worker, 0u, PWOS_JOB_KERNEL_MATMUL,
        input, sizeof(input), &job_id) == PWOS_JOB_STATUS_OK);
    run_until_terminal(&worker, job_id, &snapshot);
    assert(snapshot.state == PWOS_JOB_STATE_DONE);
    assert(snapshot.progress_permille == PWOS_JOB_PROGRESS_MAX);
    assert(pwos_compute_worker_get_result(
        &worker, 0u, job_id, result, sizeof(result),
        &result_len, &snapshot) == PWOS_JOB_STATUS_OK);
    assert(result_len == 20u);
    for (i = 0u; i < 4u; ++i) {
        assert(pwos_job_get_i32(result + 4u + i * 4u) == expected[i]);
    }
}

static void test_hash_and_vector(void)
{
    pwos_compute_worker_t worker;
    pwos_compute_job_snapshot_t snapshot;
    uint8_t vector[18];
    uint8_t result[64];
    uint16_t result_len;
    uint32_t hash_job;
    uint32_t vector_job;
    size_t i;

    assert(pwos_compute_worker_init(&worker, NULL) == 0);
    assert(pwos_compute_worker_submit(
        &worker, 0u, PWOS_JOB_KERNEL_HASH,
        (const uint8_t *)"abc", 3u, &hash_job) == PWOS_JOB_STATUS_OK);
    run_until_terminal(&worker, hash_job, &snapshot);
    assert(pwos_compute_worker_get_result(
        &worker, 0u, hash_job, result, sizeof(result),
        &result_len, NULL) == PWOS_JOB_STATUS_OK);
    assert(pwos_job_get_le32(result) == 0x1A47E90Bu);

    pwos_job_put_le16(vector, 4u);
    for (i = 0u; i < 4u; ++i) {
        pwos_job_put_i16(vector + 2u + i * 2u, (int16_t)(i + 1u));
        pwos_job_put_i16(vector + 10u + i * 2u, (int16_t)(10u * (i + 1u)));
    }
    assert(pwos_compute_worker_submit(
        &worker, 0u, PWOS_JOB_KERNEL_VECTOR_ADD,
        vector, sizeof(vector), &vector_job) == PWOS_JOB_STATUS_OK);
    run_until_terminal(&worker, vector_job, &snapshot);
    assert(pwos_compute_worker_get_result(
        &worker, 0u, vector_job, result, sizeof(result),
        &result_len, NULL) == PWOS_JOB_STATUS_OK);
    assert(result_len == 18u);
    for (i = 0u; i < 4u; ++i) {
        assert(pwos_job_get_i32(result + 2u + i * 4u) ==
            (int32_t)(11u * (i + 1u)));
    }
}

static void test_mandelbrot_progress_and_cancel(void)
{
    pwos_compute_worker_t worker;
    pwos_compute_job_snapshot_t snapshot;
    uint8_t input[20] = {16u, 16u, 64u, 0u};
    uint32_t job_id;
    size_t i;

    assert(pwos_compute_worker_init(&worker, NULL) == 0);
    pwos_job_put_i32(input + 4u, -2 * 65536);
    pwos_job_put_i32(input + 8u, -65536);
    pwos_job_put_i32(input + 12u, (3 * 65536) / 15);
    pwos_job_put_i32(input + 16u, (2 * 65536) / 15);
    assert(pwos_compute_worker_submit(
        &worker, 0u, PWOS_JOB_KERNEL_MANDELBROT,
        input, sizeof(input), &job_id) == PWOS_JOB_STATUS_OK);
    for (i = 0u; i < 10u; ++i) {
        assert(pwos_compute_worker_step(&worker) == 1);
    }
    assert(pwos_compute_worker_get_snapshot(
        &worker, 0u, job_id, &snapshot) == PWOS_JOB_STATUS_OK);
    assert(snapshot.state == PWOS_JOB_STATE_RUNNING);
    assert(snapshot.progress_permille > 0u &&
        snapshot.progress_permille < PWOS_JOB_PROGRESS_MAX);
    assert(pwos_compute_worker_cancel(
        &worker, 0u, job_id, &snapshot) == PWOS_JOB_STATUS_OK);
    assert(snapshot.state == PWOS_JOB_STATE_CANCELLED);
}

static void test_raytrace_tile(void)
{
    pwos_compute_worker_t worker;
    pwos_compute_job_snapshot_t snapshot;
    /* 协议 v3: 22 字节请求, tile_x/tile_y/image_w/image_h 均为 le16 */
    uint8_t input[22] = {
        3u, 1u,                /* version=3, scene=1 */
        0u, 0u, 0u, 0u,         /* tile_x=0, tile_y=0 */
        16u, 7u,                /* tile_w=16, tile_h=7 */
        0xF0u, 0x00u, 0x40u, 0x01u,  /* image_w=240, image_h=320 (le16) */
        4u, 4u,                 /* samples=4, max_depth=4 */
        1u, 0u,                 /* frame_id=1 */
        0x78u, 0x56u, 0x34u, 0x12u,  /* seed */
        0u, 0u,                 /* camera_phase=0 */
    };
    uint8_t result[260];
    uint16_t result_len = sizeof(result);
    uint32_t job_id;
    size_t i;
    int any_nonzero = 0;

    assert(pwos_compute_worker_init(&worker, NULL) == 0);
    assert(pwos_compute_worker_submit(
        &worker, 0u, PWOS_JOB_KERNEL_RAYTRACE_TILE,
        input, sizeof(input), &job_id) == PWOS_JOB_STATUS_OK);
    run_until_terminal(&worker, job_id, &snapshot);
    assert(snapshot.state == PWOS_JOB_STATE_DONE);
    assert(pwos_compute_worker_get_result(
        &worker, 0u, job_id, result, sizeof(result),
        &result_len, NULL) == PWOS_JOB_STATUS_OK);
    assert(result_len == 240u);
    for (i = 12u; i < result_len; ++i) {
        if (result[i] != 0u) any_nonzero = 1;
    }
    assert(any_nonzero);
}

int main(void)
{
    test_matmul();
    test_hash_and_vector();
    test_mandelbrot_progress_and_cancel();
    test_raytrace_tile();
    puts("pwos compute worker tests passed");
    return 0;
}
