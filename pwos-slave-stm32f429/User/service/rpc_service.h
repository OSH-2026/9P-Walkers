#ifndef PWOS_RPC_SERVICE_H
#define PWOS_RPC_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_rpc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_RPC_SERVICE_MAX_METHODS 12u
#define PWOS_RPC_SERVICE_MAX_PENDING 4u
#define PWOS_RPC_SERVICE_DEFERRED_PAYLOAD_CAP PWOS_RPC_MAX_PAYLOAD_LEN
#define PWOS_RPC_SERVICE_STREAM_CHUNK_CAP 32u
#define PWOS_RPC_SERVICE_STREAM_INTERVAL_MS 10u

typedef int (*pwos_rpc_service_send_fn)(
    void *ctx,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len);

typedef uint32_t (*pwos_rpc_service_now_fn)(void *ctx);

typedef struct {
    const uint8_t *payload;
    uint16_t payload_len;
    uint8_t src_addr;
    uint16_t call_id;
    uint32_t deadline_ms;
    uint8_t flags;
} pwos_rpc_request_t;

typedef struct {
    uint8_t *payload;
    uint16_t payload_cap;
    uint16_t payload_len;
    uint32_t defer_ms;
} pwos_rpc_result_t;

typedef uint16_t (*pwos_rpc_method_fn)(
    void *ctx,
    const pwos_rpc_request_t *request,
    pwos_rpc_result_t *result);

typedef struct {
    const char *service;
    const char *method;
    pwos_rpc_method_fn handler;
    void *handler_ctx;
} pwos_rpc_method_t;

typedef struct {
    uint8_t used;
    uint8_t has_deadline;
    uint8_t streaming;
    uint8_t src_addr;
    uint16_t call_id;
    uint16_t status;
    uint32_t due_ms;
    uint32_t deadline_at_ms;
    uint16_t payload_len;
    uint16_t stream_offset;
    uint8_t payload[PWOS_RPC_SERVICE_DEFERRED_PAYLOAD_CAP];
} pwos_rpc_pending_call_t;

typedef struct {
    uint32_t request_rx;
    uint32_t response_tx;
    uint32_t oneway_rx;
    uint32_t stream_request_rx;
    uint32_t stream_chunk_tx;
    uint32_t stream_end_tx;
    uint32_t cancel_rx;
    uint32_t deadline_tx;
    uint32_t completed;
    uint32_t bad_frames;
    uint32_t not_found;
    uint32_t busy;
    uint32_t send_failures;
    uint32_t notify_count;
    uint32_t pending_peak;
    uint16_t last_call_id;
    uint16_t last_status;
} pwos_rpc_service_stats_t;

typedef struct {
    void *io_ctx;
    pwos_rpc_service_send_fn send;
    pwos_rpc_service_now_fn now_ms;
    pwos_rpc_method_t methods[PWOS_RPC_SERVICE_MAX_METHODS];
    pwos_rpc_pending_call_t pending[PWOS_RPC_SERVICE_MAX_PENDING];
    uint8_t method_count;
    uint8_t tx_frame[PWOS_RPC_MAX_FRAME_LEN];
    uint8_t handler_output[PWOS_RPC_MAX_PAYLOAD_LEN];
    pwos_rpc_service_stats_t stats;
} pwos_rpc_service_t;

int pwos_rpc_service_init(
    pwos_rpc_service_t *service,
    void *io_ctx,
    pwos_rpc_service_send_fn send,
    pwos_rpc_service_now_fn now_ms);

int pwos_rpc_service_register(
    pwos_rpc_service_t *service,
    const char *service_name,
    const char *method_name,
    pwos_rpc_method_fn handler,
    void *handler_ctx);

int pwos_rpc_service_process(
    pwos_rpc_service_t *service,
    uint8_t src_addr,
    const uint8_t *frame,
    uint16_t frame_len);

void pwos_rpc_service_poll(pwos_rpc_service_t *service);

void pwos_rpc_service_get_stats(
    const pwos_rpc_service_t *service,
    pwos_rpc_service_stats_t *out_stats);

/* 注册 system.ping/stream/info/notify/delay/fail。 */
int pwos_rpc_service_register_builtins(pwos_rpc_service_t *service);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_RPC_SERVICE_H */
