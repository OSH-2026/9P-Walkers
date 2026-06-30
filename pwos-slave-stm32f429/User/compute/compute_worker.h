#ifndef PWOS_COMPUTE_WORKER_H
#define PWOS_COMPUTE_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_job_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_COMPUTE_MAX_JOBS 4u
#define PWOS_COMPUTE_INPUT_CAP 320u
#define PWOS_COMPUTE_RESULT_CAP 320u
#define PWOS_COMPUTE_LOG_CAP 48u

typedef void (*pwos_compute_lock_fn)(void *ctx);

typedef struct {
    void *lock_ctx;
    pwos_compute_lock_fn lock;
    pwos_compute_lock_fn unlock;
} pwos_compute_worker_config_t;

typedef struct {
    uint8_t used;
    uint8_t owner_addr;
    uint8_t kernel;
    uint8_t state;
    uint32_t job_id;
    uint16_t progress_permille;
    uint16_t input_len;
    uint16_t result_len;
    uint32_t sequence;
    uint32_t work_index;
    uint32_t work_total;
    uint32_t accumulator;
    uint8_t input[PWOS_COMPUTE_INPUT_CAP];
    uint8_t result[PWOS_COMPUTE_RESULT_CAP];
    char log[PWOS_COMPUTE_LOG_CAP];
} pwos_compute_job_t;

typedef struct {
    uint8_t state;
    uint8_t kernel;
    uint8_t owner_addr;
    uint32_t job_id;
    uint16_t progress_permille;
    uint16_t result_len;
    char log[PWOS_COMPUTE_LOG_CAP];
} pwos_compute_job_snapshot_t;

typedef struct {
    uint32_t submitted;
    uint32_t started;
    uint32_t completed;
    uint32_t failed;
    uint32_t cancelled;
    uint32_t rejected;
    uint32_t steps;
    uint32_t slots_reused;
    uint8_t active_jobs;
    uint8_t queued_jobs;
} pwos_compute_worker_stats_t;

typedef struct {
    pwos_compute_worker_config_t config;
    pwos_compute_job_t jobs[PWOS_COMPUTE_MAX_JOBS];
    uint32_t next_job_id;
    uint32_t next_sequence;
    pwos_compute_worker_stats_t stats;
} pwos_compute_worker_t;

int pwos_compute_worker_init(
    pwos_compute_worker_t *worker,
    const pwos_compute_worker_config_t *config);

uint16_t pwos_compute_worker_submit(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint8_t kernel,
    const uint8_t *input,
    uint16_t input_len,
    uint32_t *out_job_id);

uint16_t pwos_compute_worker_get_snapshot(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint32_t job_id,
    pwos_compute_job_snapshot_t *out_snapshot);

uint16_t pwos_compute_worker_get_result(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint32_t job_id,
    uint8_t *out_result,
    uint16_t out_cap,
    uint16_t *out_len,
    pwos_compute_job_snapshot_t *out_snapshot);

uint16_t pwos_compute_worker_cancel(
    pwos_compute_worker_t *worker,
    uint8_t owner_addr,
    uint32_t job_id,
    pwos_compute_job_snapshot_t *out_snapshot);

int pwos_compute_worker_get_slot_snapshot(
    pwos_compute_worker_t *worker,
    size_t index,
    pwos_compute_job_snapshot_t *out_snapshot);

/* 每次只推进一个最小工作单元；返回 1 表示做了工作，0 表示当前空闲。 */
int pwos_compute_worker_step(pwos_compute_worker_t *worker);

void pwos_compute_worker_get_stats(
    pwos_compute_worker_t *worker,
    pwos_compute_worker_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_COMPUTE_WORKER_H */
