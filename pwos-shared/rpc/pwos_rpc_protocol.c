#include "pwos_rpc_protocol.h"

#include <string.h>

enum {
    RPC_OFF_VERSION = 0,
    RPC_OFF_KIND = 1,
    RPC_OFF_FLAGS = 2,
    RPC_OFF_HEADER_LEN = 3,
    RPC_OFF_CALL_ID = 4,
    RPC_OFF_STATUS = 6,
    RPC_OFF_DEADLINE = 8,
    RPC_OFF_SERVICE_LEN = 12,
    RPC_OFF_METHOD_LEN = 13,
    RPC_OFF_PAYLOAD_LEN = 14,
};

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)(value >> 24);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

static int kind_valid(uint8_t kind)
{
    return kind >= PWOS_RPC_KIND_REQUEST && kind <= PWOS_RPC_KIND_STREAM_END;
}

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
    size_t *out_len)
{
    size_t service_len = service == NULL ? 0u : strlen(service);
    size_t method_len = method == NULL ? 0u : strlen(method);
    size_t total;
    size_t offset;

    if (out == NULL || out_len == NULL || !kind_valid(kind) ||
        service_len > PWOS_RPC_MAX_NAME_LEN || method_len > PWOS_RPC_MAX_NAME_LEN ||
        payload_len > PWOS_RPC_MAX_PAYLOAD_LEN ||
        (payload_len > 0u && payload == NULL)) {
        return -1;
    }
    /* 只有 REQUEST 携带 service/method，响应、cancel、stream chunk/end 不携带名称。 */
    if (kind == PWOS_RPC_KIND_REQUEST &&
        (service_len == 0u || method_len == 0u)) {
        return -1;
    }
    if (kind != PWOS_RPC_KIND_REQUEST &&
        (service_len != 0u || method_len != 0u)) {
        return -1;
    }
    total = PWOS_RPC_HEADER_LEN + service_len + method_len + payload_len;
    if (total > out_cap || total > PWOS_RPC_MAX_FRAME_LEN) {
        return -1;
    }

    /* 头部固定 16 字节，后面紧跟 service、method、payload 三段变长区域。 */
    memset(out, 0, PWOS_RPC_HEADER_LEN);
    out[RPC_OFF_VERSION] = PWOS_RPC_VERSION;
    out[RPC_OFF_KIND] = kind;
    out[RPC_OFF_FLAGS] = flags;
    out[RPC_OFF_HEADER_LEN] = PWOS_RPC_HEADER_LEN;
    put_le16(out + RPC_OFF_CALL_ID, call_id);
    put_le16(out + RPC_OFF_STATUS, status);
    put_le32(out + RPC_OFF_DEADLINE, deadline_ms);
    out[RPC_OFF_SERVICE_LEN] = (uint8_t)service_len;
    out[RPC_OFF_METHOD_LEN] = (uint8_t)method_len;
    put_le16(out + RPC_OFF_PAYLOAD_LEN, payload_len);

    offset = PWOS_RPC_HEADER_LEN;
    if (service_len > 0u) {
        memcpy(out + offset, service, service_len);
        offset += service_len;
    }
    if (method_len > 0u) {
        memcpy(out + offset, method, method_len);
        offset += method_len;
    }
    if (payload_len > 0u) {
        memcpy(out + offset, payload, payload_len);
    }
    *out_len = total;
    return 0;
}

int pwos_rpc_decode(
    const uint8_t *frame,
    size_t frame_len,
    pwos_rpc_frame_view_t *out_view)
{
    size_t expected;
    size_t offset;

    if (frame == NULL || out_view == NULL || frame_len < PWOS_RPC_HEADER_LEN ||
        frame_len > PWOS_RPC_MAX_FRAME_LEN ||
        frame[RPC_OFF_VERSION] != PWOS_RPC_VERSION ||
        frame[RPC_OFF_HEADER_LEN] != PWOS_RPC_HEADER_LEN ||
        !kind_valid(frame[RPC_OFF_KIND]) ||
        frame[RPC_OFF_SERVICE_LEN] > PWOS_RPC_MAX_NAME_LEN ||
        frame[RPC_OFF_METHOD_LEN] > PWOS_RPC_MAX_NAME_LEN) {
        return -1;
    }
    expected = PWOS_RPC_HEADER_LEN + frame[RPC_OFF_SERVICE_LEN] +
        frame[RPC_OFF_METHOD_LEN] + get_le16(frame + RPC_OFF_PAYLOAD_LEN);
    /* 长度必须精确匹配，防止截断帧或多余字节被误接受。 */
    if (expected != frame_len ||
        (frame[RPC_OFF_KIND] == PWOS_RPC_KIND_REQUEST &&
         (frame[RPC_OFF_SERVICE_LEN] == 0u || frame[RPC_OFF_METHOD_LEN] == 0u)) ||
        (frame[RPC_OFF_KIND] != PWOS_RPC_KIND_REQUEST &&
         (frame[RPC_OFF_SERVICE_LEN] != 0u || frame[RPC_OFF_METHOD_LEN] != 0u))) {
        return -1;
    }

    memset(out_view, 0, sizeof(*out_view));
    out_view->kind = frame[RPC_OFF_KIND];
    out_view->flags = frame[RPC_OFF_FLAGS];
    out_view->call_id = get_le16(frame + RPC_OFF_CALL_ID);
    out_view->status = get_le16(frame + RPC_OFF_STATUS);
    out_view->deadline_ms = get_le32(frame + RPC_OFF_DEADLINE);
    out_view->service_len = frame[RPC_OFF_SERVICE_LEN];
    out_view->method_len = frame[RPC_OFF_METHOD_LEN];
    out_view->payload_len = get_le16(frame + RPC_OFF_PAYLOAD_LEN);
    offset = PWOS_RPC_HEADER_LEN;
    out_view->service = frame + offset;
    offset += out_view->service_len;
    out_view->method = frame + offset;
    offset += out_view->method_len;
    out_view->payload = frame + offset;
    return 0;
}

int pwos_rpc_retag(uint8_t *frame, size_t frame_len, uint16_t call_id)
{
    pwos_rpc_frame_view_t view;

    if (pwos_rpc_decode(frame, frame_len, &view) != 0) {
        return -1;
    }
    /* RPC 内层没有 CRC，完整性由外层 link payload CRC 负责。 */
    put_le16(frame + RPC_OFF_CALL_ID, call_id);
    return 0;
}

int pwos_rpc_name_equals(
    const uint8_t *wire_name,
    uint8_t wire_len,
    const char *name)
{
    size_t name_len;

    if (wire_name == NULL || name == NULL) {
        return 0;
    }
    name_len = strlen(name);
    return name_len == wire_len && memcmp(wire_name, name, wire_len) == 0;
}

const char *pwos_rpc_status_name(uint16_t status)
{
    switch (status) {
    case PWOS_RPC_STATUS_OK:
        return "ok";
    case PWOS_RPC_STATUS_BAD_REQUEST:
        return "bad_request";
    case PWOS_RPC_STATUS_NOT_FOUND:
        return "not_found";
    case PWOS_RPC_STATUS_DEADLINE:
        return "deadline";
    case PWOS_RPC_STATUS_CANCELLED:
        return "cancelled";
    case PWOS_RPC_STATUS_BUSY:
        return "busy";
    case PWOS_RPC_STATUS_INTERNAL:
        return "internal";
    case PWOS_RPC_STATUS_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown";
    }
}
