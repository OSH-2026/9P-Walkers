#ifndef PWOS_JOB_COMMAND_H
#define PWOS_JOB_COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "job_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_JOB_COMMAND_MAX_LINE 256u

typedef int (*pwos_job_command_resolve_fn)(
    void *ctx,
    const char *target,
    uint8_t *out_addr,
    uint32_t *out_boot_id);

typedef struct {
    pwos_job_manager_t *manager;
    void *resolve_ctx;
    pwos_job_command_resolve_fn resolve;
} pwos_job_command_config_t;

typedef struct {
    pwos_job_command_config_t config;
} pwos_job_command_t;

int pwos_job_command_init(
    pwos_job_command_t *command,
    const pwos_job_command_config_t *config);

/* args 是去掉开头 "job" 后的命令参数。 */
int pwos_job_command_execute(
    pwos_job_command_t *command,
    const char *args,
    char *output,
    size_t output_cap,
    size_t *out_len,
    uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_JOB_COMMAND_H */
