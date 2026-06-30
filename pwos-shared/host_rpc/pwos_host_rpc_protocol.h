#ifndef PWOS_HOST_RPC_PROTOCOL_H
#define PWOS_HOST_RPC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_HOST_RPC_VERSION 1u
#define PWOS_HOST_RPC_PREFIX_LEN 4u
#define PWOS_HOST_RPC_MAX_SERVICE_LEN 31u
#define PWOS_HOST_RPC_MAX_METHOD_LEN 31u
#define PWOS_HOST_RPC_MAX_PAYLOAD_LEN 4096u
#define PWOS_HOST_RPC_MAX_FRAME_LEN 1280u

typedef enum {
    PWOS_HOST_RPC_KIND_REQUEST = 1,
    PWOS_HOST_RPC_KIND_RESPONSE = 2,
    PWOS_HOST_RPC_KIND_CANCEL = 3,
    PWOS_HOST_RPC_KIND_STREAM_CHUNK = 4,
    PWOS_HOST_RPC_KIND_STREAM_END = 5,
} pwos_host_rpc_kind_t;

typedef enum {
    PWOS_HOST_RPC_STATUS_OK = 0,
    PWOS_HOST_RPC_STATUS_BAD_REQUEST = 1,
    PWOS_HOST_RPC_STATUS_NOT_FOUND = 2,
    PWOS_HOST_RPC_STATUS_DEADLINE = 3,
    PWOS_HOST_RPC_STATUS_BUSY = 4,
    PWOS_HOST_RPC_STATUS_INTERNAL = 5,
    PWOS_HOST_RPC_STATUS_NOT_LEADER = 6,
    PWOS_HOST_RPC_STATUS_NO_ROUTE = 7,
    PWOS_HOST_RPC_STATUS_CANCELLED = 8,
} pwos_host_rpc_status_t;

typedef struct {
    uint8_t kind;
    uint32_t call_id;
    uint32_t deadline_ms;
    uint16_t status;
    const char *service;
    uint8_t service_len;
    const char *method;
    uint8_t method_len;
    const uint8_t *payload;
    uint16_t payload_len;
} pwos_host_rpc_frame_view_t;

/* 输出包含 4 字节网络序 CBOR body 长度前缀，便于 TCP 精确收帧。 */
int pwos_host_rpc_encode(
    uint8_t kind,
    uint32_t call_id,
    uint32_t deadline_ms,
    uint16_t status,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len);

int pwos_host_rpc_decode(
    const uint8_t *frame,
    size_t frame_len,
    pwos_host_rpc_frame_view_t *out_view);

uint32_t pwos_host_rpc_body_len(const uint8_t prefix[PWOS_HOST_RPC_PREFIX_LEN]);

const char *pwos_host_rpc_status_name(uint16_t status);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_PROTOCOL_H */
