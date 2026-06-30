#ifndef PWOS_RPC_PROTOCOL_H
#define PWOS_RPC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_RPC_VERSION 1u
#define PWOS_RPC_HEADER_LEN 16u
#define PWOS_RPC_MAX_NAME_LEN 31u
#define PWOS_RPC_MAX_FRAME_LEN 512u
#define PWOS_RPC_MAX_PAYLOAD_LEN \
    (PWOS_RPC_MAX_FRAME_LEN - PWOS_RPC_HEADER_LEN - 2u * PWOS_RPC_MAX_NAME_LEN)

#define PWOS_RPC_FLAG_ONEWAY 0x01u
#define PWOS_RPC_FLAG_STREAM 0x02u

/* STREAM_CHUNK 的 status 字段承载从 0 开始的 chunk 序号。 */

typedef enum {
    PWOS_RPC_KIND_REQUEST = 1,
    PWOS_RPC_KIND_RESPONSE = 2,
    PWOS_RPC_KIND_CANCEL = 3,
    PWOS_RPC_KIND_STREAM_CHUNK = 4,
    PWOS_RPC_KIND_STREAM_END = 5,
} pwos_rpc_kind_t;

typedef enum {
    PWOS_RPC_STATUS_OK = 0,
    PWOS_RPC_STATUS_BAD_REQUEST = 1,
    PWOS_RPC_STATUS_NOT_FOUND = 2,
    PWOS_RPC_STATUS_DEADLINE = 3,
    PWOS_RPC_STATUS_CANCELLED = 4,
    PWOS_RPC_STATUS_BUSY = 5,
    PWOS_RPC_STATUS_INTERNAL = 6,
    PWOS_RPC_STATUS_UNSUPPORTED = 7,
} pwos_rpc_status_t;

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint16_t call_id;
    uint16_t status;
    uint32_t deadline_ms;
    const uint8_t *service;
    uint8_t service_len;
    const uint8_t *method;
    uint8_t method_len;
    const uint8_t *payload;
    uint16_t payload_len;
} pwos_rpc_frame_view_t;

/* 编码通用 RPC 帧；字符串字段不包含结尾 NUL。 */
int pwos_rpc_encode(
    uint8_t kind,
    uint8_t flags,
    uint16_t call_id,
    uint16_t status,
    uint32_t deadline_ms,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len);

int pwos_rpc_decode(
    const uint8_t *frame,
    size_t frame_len,
    pwos_rpc_frame_view_t *out_view);

/* 修改 call_id。RPC 完整性由外层 link payload CRC 保证，无需重算内层 CRC。 */
int pwos_rpc_retag(uint8_t *frame, size_t frame_len, uint16_t call_id);

int pwos_rpc_name_equals(
    const uint8_t *wire_name,
    uint8_t wire_len,
    const char *name);

const char *pwos_rpc_status_name(uint16_t status);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_RPC_PROTOCOL_H */
