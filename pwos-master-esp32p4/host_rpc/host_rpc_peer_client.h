#ifndef PWOS_HOST_RPC_PEER_CLIENT_H
#define PWOS_HOST_RPC_PEER_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_host_rpc_methods.h"
#include "pwos_host_rpc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_HOST_RPC_REMOTE_ERROR_BASE 2000

typedef int (*pwos_host_rpc_exchange_fn)(
    void *ctx,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_cap,
    size_t *out_response_len,
    uint32_t deadline_ms);

typedef void (*pwos_host_rpc_client_lock_fn)(void *ctx);

typedef struct {
    void *io_ctx;
    pwos_host_rpc_exchange_fn exchange;
    void *lock_ctx;
    pwos_host_rpc_client_lock_fn lock;
    pwos_host_rpc_client_lock_fn unlock;
} pwos_host_rpc_peer_client_config_t;

typedef struct {
    uint32_t calls;
    uint32_t responses;
    uint32_t transport_errors;
    uint32_t remote_errors;
    uint32_t malformed_responses;
    uint32_t last_call_id;
    uint16_t last_status;
    int32_t last_error;
} pwos_host_rpc_peer_client_stats_t;

typedef struct {
    pwos_host_rpc_peer_client_config_t config;
    uint32_t next_call_id;
    pwos_host_rpc_peer_client_stats_t stats;
} pwos_host_rpc_peer_client_t;

int pwos_host_rpc_peer_client_init(
    pwos_host_rpc_peer_client_t *client,
    const pwos_host_rpc_peer_client_config_t *config);

int pwos_host_rpc_peer_client_call(
    pwos_host_rpc_peer_client_t *client,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response_payload,
    uint16_t *in_out_response_len,
    uint16_t *out_status);

int pwos_host_rpc_peer_client_read_node(
    pwos_host_rpc_peer_client_t *client,
    const char *target,
    const char *path,
    uint8_t *data,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

int pwos_host_rpc_peer_client_write_node(
    pwos_host_rpc_peer_client_t *client,
    const char *target,
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms);

void pwos_host_rpc_peer_client_get_stats(
    pwos_host_rpc_peer_client_t *client,
    pwos_host_rpc_peer_client_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_PEER_CLIENT_H */
