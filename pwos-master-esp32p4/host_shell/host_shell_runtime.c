#include "host_shell_runtime.h"

#include <stdio.h>
#include <string.h>

#include "host_observability.h"
#include "pwos_coordinator_runtime.h"

#define PWOS_HOST_PERM_READ 0x01u
#define PWOS_HOST_ROOT_QID 0x6FFFu

static int is_host_path(const char *path)
{
    return path != NULL &&
        (strcmp(path, "/host") == 0 || strncmp(path, "/host/", 6u) == 0);
}

static int shell_read(
    void *ctx,
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    (void)ctx;
    if (is_host_path(path)) {
        return pwos_host_observability_read(path, buf, in_out_len);
    }
    return pwos_coordinator_runtime_read_path(
        path, buf, in_out_len, deadline_ms);
}

static int shell_write(
    void *ctx,
    const char *path,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    (void)ctx;
    if (is_host_path(path)) {
        return -(int)M9P_ERR_EPERM;
    }
    return pwos_coordinator_runtime_write_path(
        path, data, len, out_written, deadline_ms);
}

static int shell_list(
    void *ctx,
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t deadline_ms)
{
    size_t remote_count = 0u;
    int rc;

    (void)ctx;
    if (path == NULL || out_count == NULL ||
        (max_entries > 0u && entries == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (is_host_path(path)) {
        return pwos_host_observability_list(
            path, entries, max_entries, out_count);
    }
    if (strcmp(path, "/") != 0) {
        return pwos_coordinator_runtime_list(
            path, entries, max_entries, out_count, deadline_ms);
    }

    *out_count = 0u;
    if (max_entries == 0u) {
        return 0;
    }
    memset(&entries[0], 0, sizeof(entries[0]));
    entries[0].qid.type = (uint8_t)(M9P_QID_DIR | M9P_QID_VIRTUAL);
    entries[0].qid.object_id = 0x7000u;
    entries[0].perm = PWOS_HOST_PERM_READ;
    entries[0].flags = (uint8_t)(M9P_STAT_DIR | M9P_STAT_VIRTUAL);
    (void)snprintf(entries[0].name, sizeof(entries[0].name), "host");
    *out_count = 1u;
    if (max_entries == 1u) {
        return 0;
    }

    rc = pwos_coordinator_runtime_list(
        path,
        entries + 1u,
        max_entries - 1u,
        &remote_count,
        deadline_ms);
    if (rc != 0) {
        return rc;
    }
    *out_count += remote_count;
    return 0;
}

static int shell_stat(
    void *ctx,
    const char *path,
    struct m9p_stat *out_stat,
    uint32_t deadline_ms)
{
    (void)ctx;
    if (path == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (strcmp(path, "/") == 0) {
        memset(out_stat, 0, sizeof(*out_stat));
        out_stat->qid.type = (uint8_t)(M9P_QID_DIR | M9P_QID_VIRTUAL);
        out_stat->qid.object_id = PWOS_HOST_ROOT_QID;
        out_stat->perm = PWOS_HOST_PERM_READ;
        out_stat->flags = (uint8_t)(M9P_STAT_DIR | M9P_STAT_VIRTUAL);
        (void)snprintf(out_stat->name, sizeof(out_stat->name), "/");
        return 0;
    }
    if (is_host_path(path)) {
        return pwos_host_observability_stat(path, out_stat);
    }
    return pwos_coordinator_runtime_stat(path, out_stat, deadline_ms);
}

static int shell_rpc(
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
    uint16_t *out_chunk_count)
{
    (void)ctx;
    if (mode == PWOS_COMMAND_RPC_ONEWAY) {
        return pwos_coordinator_runtime_rpc_notify(
            target, service, method, payload, payload_len, deadline_ms);
    }
    if (mode == PWOS_COMMAND_RPC_STREAM) {
        return pwos_coordinator_runtime_rpc_stream(
            target,
            service,
            method,
            payload,
            payload_len,
            deadline_ms,
            response,
            in_out_response_len,
            out_status,
            out_chunk_count);
    }
    return pwos_coordinator_runtime_rpc_call(
        target,
        service,
        method,
        payload,
        payload_len,
        deadline_ms,
        response,
        in_out_response_len,
        out_status);
}

int pwos_host_shell_runtime_build_config(
    pwos_command_service_config_t *out_config)
{
    if (out_config == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    memset(out_config, 0, sizeof(*out_config));
    out_config->read_path = shell_read;
    out_config->write_path = shell_write;
    out_config->list = shell_list;
    out_config->stat = shell_stat;
    out_config->rpc = shell_rpc;
    out_config->default_deadline_ms = PWOS_COMMAND_DEFAULT_DEADLINE_MS;
    return 0;
}
