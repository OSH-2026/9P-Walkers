#ifndef PWOS_JOB_MANAGER_H
#define PWOS_JOB_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_job_protocol.h"
#include "session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_JOB_MANAGER_MAX_JOBS 16u
#define PWOS_JOB_MANAGER_TARGET_CAP 16u
#define PWOS_JOB_MANAGER_DEFAULT_DEADLINE_MS 1000u

typedef void (*pwos_job_manager_lock_fn)(void *ctx);

typedef struct {
    void *lock_ctx;
    pwos_job_manager_lock_fn lock;
    pwos_job_manager_lock_fn unlock;
} pwos_job_manager_config_t;

typedef struct {
    uint8_t used;
    uint8_t addr;
    uint8_t kernel;
    uint8_t state;
    char target[PWOS_JOB_MANAGER_TARGET_CAP];
    uint32_t boot_id;
    uint32_t host_job_id;
    uint32_t remote_job_id;
    uint16_t progress_permille;
    uint16_t input_len;
    uint32_t result_len;
    int32_t last_error;
    uint16_t remote_status;
    uint32_t sequence;
    uint8_t input[PWOS_JOB_MAX_PAYLOAD_LEN];
} pwos_job_entry_t;

typedef struct {
    uint32_t caps_requests;
    uint32_t submitted;
    uint32_t status_requests;
    uint32_t result_requests;
    uint32_t cancelled;
    uint32_t retried;
    uint32_t lost;
    uint32_t transport_errors;
    uint32_t remote_errors;
    uint32_t slots_reused;
    uint32_t last_host_job_id;
    int32_t last_error;
} pwos_job_manager_stats_t;

typedef struct {
    pwos_session_manager_t *sessions;
    pwos_job_manager_config_t config;
    pwos_job_entry_t jobs[PWOS_JOB_MANAGER_MAX_JOBS];
    uint32_t next_host_job_id;
    uint32_t next_sequence;
    pwos_job_manager_stats_t stats;
} pwos_job_manager_t;

int pwos_job_manager_init(
    pwos_job_manager_t *manager,
    pwos_session_manager_t *sessions,
    const pwos_job_manager_config_t *config);

int pwos_job_manager_caps(
    pwos_job_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t *out_caps,
    uint16_t *in_out_len,
    uint32_t deadline_ms,
    uint16_t *out_remote_status);

int pwos_job_manager_submit(
    pwos_job_manager_t *manager,
    const char *target,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t kernel,
    const uint8_t *input,
    uint16_t input_len,
    uint32_t deadline_ms,
    uint32_t *out_host_job_id,
    uint16_t *out_remote_status);

int pwos_job_manager_status(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint32_t deadline_ms,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status);

int pwos_job_manager_result(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint32_t deadline_ms,
    uint8_t *out_result,
    uint16_t *in_out_len,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status);

int pwos_job_manager_cancel(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint32_t deadline_ms,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status);

int pwos_job_manager_retry(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms,
    uint32_t *out_new_host_job_id,
    uint16_t *out_remote_status);

int pwos_job_manager_get(
    pwos_job_manager_t *manager,
    uint32_t host_job_id,
    pwos_job_entry_t *out_entry);

int pwos_job_manager_get_at(
    pwos_job_manager_t *manager,
    size_t index,
    pwos_job_entry_t *out_entry);

void pwos_job_manager_get_stats(
    pwos_job_manager_t *manager,
    pwos_job_manager_stats_t *out_stats);

/* 节点重启或离线时，将尚未结束的 job 统一转为 LOST。 */
size_t pwos_job_manager_mark_node_lost(
    pwos_job_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    int32_t error);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_JOB_MANAGER_H */
