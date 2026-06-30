#ifndef PWOS_RPC_CLIENT_H
#define PWOS_RPC_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_rpc_protocol.h"
#include "session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_RPC_CLIENT_DEFAULT_DEADLINE_MS 1000u
#define PWOS_RPC_CLIENT_DEADLINE_GRACE_MS 100u

typedef struct {
    uint32_t unary_tx;
    uint32_t unary_rx;
    uint32_t oneway_tx;
    uint32_t stream_tx;
    uint32_t stream_rx;
    uint32_t stream_chunks_rx;
    uint32_t cancel_tx;
    uint32_t deadline_errors;
    uint32_t malformed_responses;
    uint32_t remote_errors;
    int32_t last_error;
    uint16_t last_status;
    uint16_t last_call_id;
} pwos_rpc_client_stats_t;

typedef struct {
    pwos_session_manager_t *sessions;
    pwos_rpc_client_stats_t stats;
} pwos_rpc_client_t;

int pwos_rpc_client_init(
    pwos_rpc_client_t *client,
    pwos_session_manager_t *sessions);

int pwos_rpc_client_call(
    pwos_rpc_client_t *client,
    uint8_t addr,
    uint32_t boot_id,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status);

int pwos_rpc_client_notify(
    pwos_rpc_client_t *client,
    uint8_t addr,
    uint32_t boot_id,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms);

/* 聚合一个有界流；chunk 在 RX task 中追加，调用者只在 STREAM_END 后返回。 */
int pwos_rpc_client_stream(
    pwos_rpc_client_t *client,
    uint8_t addr,
    uint32_t boot_id,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status,
    uint16_t *out_chunk_count);

void pwos_rpc_client_get_stats(
    const pwos_rpc_client_t *client,
    pwos_rpc_client_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_RPC_CLIENT_H */
