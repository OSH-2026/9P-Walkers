#ifndef PWOS_COMMAND_SERVICE_H
#define PWOS_COMMAND_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_COMMAND_MAX_LINE 256u
#define PWOS_COMMAND_DEFAULT_DEADLINE_MS 1200u
#define PWOS_COMMAND_RPC_CALL 0u
#define PWOS_COMMAND_RPC_ONEWAY 1u
#define PWOS_COMMAND_RPC_STREAM 2u

typedef int (*pwos_command_read_fn)(
    void *ctx,
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

typedef int (*pwos_command_write_fn)(
    void *ctx,
    const char *path,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms);

typedef int (*pwos_command_list_fn)(
    void *ctx,
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t deadline_ms);

typedef int (*pwos_command_stat_fn)(
    void *ctx,
    const char *path,
    struct m9p_stat *out_stat,
    uint32_t deadline_ms);

typedef int (*pwos_command_rpc_fn)(
    void *ctx,
    const char *target,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t mode,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status,
    uint16_t *out_chunk_count);

typedef struct {
    void *io_ctx;
    pwos_command_read_fn read_path;
    pwos_command_write_fn write_path;
    pwos_command_list_fn list;
    pwos_command_stat_fn stat;
    pwos_command_rpc_fn rpc;
    uint32_t default_deadline_ms;
} pwos_command_service_config_t;

typedef struct {
    pwos_command_service_config_t config;
    uint32_t executed;
    uint32_t failed;
    uint32_t truncated;
} pwos_command_service_t;

int pwos_command_service_init(
    pwos_command_service_t *service,
    const pwos_command_service_config_t *config);

int pwos_command_service_execute(
    pwos_command_service_t *service,
    const char *line,
    char *output,
    size_t output_cap,
    size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_COMMAND_SERVICE_H */
