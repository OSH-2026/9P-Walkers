#include "mesh_protocal.h"

#include <string.h>

/*
 * 内部工具函数：
 * - 显式小端序读写，避免结构体对齐和平台差异问题。
 * - static 限定，避免污染外部符号空间。
 */
static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

/*
 * 安全的有界 strlen：
 * - 允许输入为空。
 * - 最多计数到 max_len，避免越界扫描。
 */
static size_t bounded_strlen(const char *str, size_t max_len)
{
    size_t len = 0u;

    if (str == NULL) {
        return 0u;
    }

    while (len < max_len && str[len] != '\0') {
        ++len;
    }
    return len;
}

/*
 * 内部封装：统一走 mesh_encode_frame，确保所有消息构帧校验逻辑一致。
 */
static bool encode_payload_to_frame(
    uint8_t type,
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    uint8_t flags,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    return mesh_encode_frame(
        type,
        src,
        dst,
        seq,
        hop,
        flags,
        payload,
        payload_len,
        out_frame,
        out_cap,
        out_len);
}

uint16_t mesh_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;

    /*
     * CRC-16/CCITT-FALSE 参数：
     * - 多项式: 0x1021
     * - 初始值: 0xFFFF
     * - 不反射，不异或输出
     */
    for (i = 0; i < len; ++i) {
        unsigned int bit;

        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

bool mesh_is_control_type(uint8_t type)
{
    return (type == MESH_TYPE_REGISTER) ||
        (type == MESH_TYPE_ASSIGN) ||
        (type == MESH_TYPE_PING) ||
        (type == MESH_TYPE_PONG) ||
        (type == MESH_TYPE_TIME_SYNC) ||
        (type == MESH_TYPE_ROUTE_UPDATE) ||
        (type == MESH_TYPE_LINK_STATE) ||
        (type == MESH_TYPE_ERROR);
}

bool mesh_encode_frame(
    uint8_t type,
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    uint8_t flags,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint16_t frame_len_field;
    size_t total_len;
    uint16_t crc;

    /* 参数和容量检查。 */
    if (out_frame == NULL || out_len == NULL) {
        return false;
    }
    if (payload_len > 0u && payload == NULL) {
        return false;
    }
    if (payload_len > MESH_MAX_PAYLOAD_LEN) {
        return false;
    }

    /* FrameLen = 固定头(8) + payload。 */
    frame_len_field = (uint16_t)(8u + payload_len);

    /* 整帧长度 = Magic(2) + FrameLen(2) + FrameLen字节 + CRC(2)。 */
    total_len = 2u + 2u + (size_t)frame_len_field + 2u;
    if (out_cap < total_len) {
        return false;
    }

    /* 写入固定头。 */
    out_frame[0] = (uint8_t)'M';
    out_frame[1] = (uint8_t)'H';
    put_le16(out_frame + 2, frame_len_field);
    out_frame[4] = MESH_VERSION;
    out_frame[5] = type;
    out_frame[6] = src;
    out_frame[7] = dst;
    put_le16(out_frame + 8, seq);
    out_frame[10] = hop;
    out_frame[11] = flags;

    if (payload_len > 0u) {
        memcpy(out_frame + 12, payload, payload_len);
    }

    /* CRC 覆盖 Version..Payload。 */
    crc = mesh_crc16_ccitt_false(out_frame + 4, frame_len_field);
    put_le16(out_frame + 12 + payload_len, crc);

    *out_len = total_len;
    return true;
}

bool mesh_decode_frame(const uint8_t *frame, size_t frame_len, struct mesh_frame_view *out_view)
{
    uint16_t frame_len_field;
    uint16_t actual_crc;
    uint16_t expected_crc;

    /* 基础格式检查。 */
    if (frame == NULL || out_view == NULL || frame_len < MESH_FRAME_OVERHEAD) {
        return false;
    }
    if (frame[0] != (uint8_t)'M' || frame[1] != (uint8_t)'H') {
        return false;
    }

    frame_len_field = get_le16(frame + 2);
    if (frame_len_field < 8u) {
        return false;
    }

    /* 仅接受完整帧，避免半包误判。 */
    if ((size_t)(frame_len_field + 6u) != frame_len) {
        return false;
    }

    actual_crc = get_le16(frame + frame_len - 2u);
    expected_crc = mesh_crc16_ccitt_false(frame + 4, frame_len_field);
    if (actual_crc != expected_crc) {
        return false;
    }

    out_view->version = frame[4];
    out_view->type = frame[5];
    out_view->src = frame[6];
    out_view->dst = frame[7];
    out_view->seq = get_le16(frame + 8);
    out_view->hop = frame[10];
    out_view->flags = frame[11];
    out_view->payload = frame + 12;
    out_view->payload_len = (uint16_t)(frame_len_field - 8u);

    /* v1 实现只接受 MESH_VERSION。 */
    if (out_view->version != MESH_VERSION) {
        return false;
    }
    return true;
}

bool mesh_prepare_forward(const struct mesh_frame_view *in_frame, uint8_t *out_hop)
{
    if (in_frame == NULL || out_hop == NULL) {
        return false;
    }

    /* hop 为剩余跳数，0 表示不可再转发。 */
    if (in_frame->hop == 0u) {
        return false;
    }

    *out_hop = (uint8_t)(in_frame->hop - 1u);
    return true;
}

bool mesh_build_mini9p_frame(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    uint8_t flags,
    const uint8_t *mini9p_frame,
    uint16_t mini9p_len,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    /* 数据面帧强制清除 CONTROL 位，避免控制/数据语义混用。 */
    return encode_payload_to_frame(
        MESH_TYPE_MINI9P,
        src,
        dst,
        seq,
        hop,
        (uint8_t)(flags & (uint8_t)~MESH_FLAG_CONTROL),
        mini9p_frame,
        mini9p_len,
        out_frame,
        out_cap,
        out_len);
}

bool mesh_parse_mini9p_payload(
    const struct mesh_frame_view *frame,
    const uint8_t **out_payload,
    uint16_t *out_len)
{
    if (frame == NULL || out_payload == NULL || out_len == NULL) {
        return false;
    }
    if (frame->type != MESH_TYPE_MINI9P) {
        return false;
    }

    *out_payload = frame->payload;
    *out_len = frame->payload_len;
    return true;
}

bool mesh_build_register(
    uint8_t src,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_register_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t raw[15];

    if (payload == NULL) {
        return false;
    }

    /* v1 REGISTER 为定长负载。 */
    memcpy(raw, payload->uid, MESH_UID_LEN);
    put_le32(raw + 8, payload->boot_nonce);
    put_le16(raw + 12, payload->capability_bits);
    raw[14] = payload->port_bitmap;

    return encode_payload_to_frame(
        MESH_TYPE_REGISTER,
        src,
        MESH_ADDR_UNASSIGNED,
        seq,
        hop,
        (uint8_t)(MESH_FLAG_CONTROL | MESH_FLAG_NEEDS_ACK),
        raw,
        (uint16_t)sizeof(raw),
        out_frame,
        out_cap,
        out_len);
}

bool mesh_parse_register(const struct mesh_frame_view *frame, struct mesh_register_payload *out_payload)
{
    const uint8_t *payload;

    if (frame == NULL || out_payload == NULL) {
        return false;
    }
    if (frame->type != MESH_TYPE_REGISTER || frame->payload_len != 15u) {
        return false;
    }

    payload = frame->payload;
    memcpy(out_payload->uid, payload, MESH_UID_LEN);
    out_payload->boot_nonce = get_le32(payload + 8);
    out_payload->capability_bits = get_le16(payload + 12);
    out_payload->port_bitmap = payload[14];
    return true;
}

bool mesh_build_assign(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_assign_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t raw[8u + 1u + 4u + 2u + 1u + MESH_MAX_NODE_NAME];
    size_t name_len;
    uint16_t raw_len;

    if (payload == NULL) {
        return false;
    }

    /* 节点名按长度前缀编码，最大 MESH_MAX_NODE_NAME。 */
    name_len = bounded_strlen(payload->node_name, MESH_MAX_NODE_NAME);
    memcpy(raw, payload->uid, MESH_UID_LEN);
    raw[8] = payload->node_addr;
    put_le32(raw + 9, payload->lease_ms);
    put_le16(raw + 13, payload->epoch);
    raw[15] = (uint8_t)name_len;
    if (name_len > 0u) {
        memcpy(raw + 16, payload->node_name, name_len);
    }
    raw_len = (uint16_t)(16u + name_len);

    return encode_payload_to_frame(
        MESH_TYPE_ASSIGN,
        src,
        dst,
        seq,
        hop,
        (uint8_t)(MESH_FLAG_CONTROL | MESH_FLAG_NEEDS_ACK),
        raw,
        raw_len,
        out_frame,
        out_cap,
        out_len);
}

bool mesh_parse_assign(const struct mesh_frame_view *frame, struct mesh_assign_payload *out_payload)
{
    const uint8_t *payload;
    uint8_t name_len;

    if (frame == NULL || out_payload == NULL) {
        return false;
    }
    if (frame->type != MESH_TYPE_ASSIGN || frame->payload_len < 16u) {
        return false;
    }

    payload = frame->payload;
    name_len = payload[15];

    /* 严格长度校验：防止坏包导致越界读取。 */
    if (name_len > MESH_MAX_NODE_NAME) {
        return false;
    }
    if ((uint16_t)(16u + name_len) != frame->payload_len) {
        return false;
    }

    memcpy(out_payload->uid, payload, MESH_UID_LEN);
    out_payload->node_addr = payload[8];
    out_payload->lease_ms = get_le32(payload + 9);
    out_payload->epoch = get_le16(payload + 13);
    if (name_len > 0u) {
        memcpy(out_payload->node_name, payload + 16, name_len);
    }
    out_payload->node_name[name_len] = '\0';
    return true;
}

bool mesh_build_ping(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    uint8_t type,
    const struct mesh_ping_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t raw[4];

    if (payload == NULL) {
        return false;
    }
    if (type != MESH_TYPE_PING && type != MESH_TYPE_PONG) {
        return false;
    }

    put_le32(raw, payload->local_time_ms);
    return encode_payload_to_frame(
        type,
        src,
        dst,
        seq,
        hop,
        MESH_FLAG_CONTROL,
        raw,
        (uint16_t)sizeof(raw),
        out_frame,
        out_cap,
        out_len);
}

bool mesh_parse_ping(const struct mesh_frame_view *frame, struct mesh_ping_payload *out_payload)
{
    if (frame == NULL || out_payload == NULL) {
        return false;
    }
    if ((frame->type != MESH_TYPE_PING && frame->type != MESH_TYPE_PONG) || frame->payload_len != 4u) {
        return false;
    }

    out_payload->local_time_ms = get_le32(frame->payload);
    return true;
}

bool mesh_build_time_sync(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_time_sync_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t raw[16];

    if (payload == NULL) {
        return false;
    }

    put_le32(raw, payload->t0_master_send);
    put_le32(raw + 4, payload->t1_slave_recv);
    put_le32(raw + 8, payload->t2_slave_send);
    put_le32(raw + 12, payload->t3_master_recv);
    return encode_payload_to_frame(
        MESH_TYPE_TIME_SYNC,
        src,
        dst,
        seq,
        hop,
        MESH_FLAG_CONTROL,
        raw,
        (uint16_t)sizeof(raw),
        out_frame,
        out_cap,
        out_len);
}

bool mesh_parse_time_sync(const struct mesh_frame_view *frame, struct mesh_time_sync_payload *out_payload)
{
    if (frame == NULL || out_payload == NULL) {
        return false;
    }
    if (frame->type != MESH_TYPE_TIME_SYNC || frame->payload_len != 16u) {
        return false;
    }

    out_payload->t0_master_send = get_le32(frame->payload);
    out_payload->t1_slave_recv = get_le32(frame->payload + 4);
    out_payload->t2_slave_send = get_le32(frame->payload + 8);
    out_payload->t3_master_recv = get_le32(frame->payload + 12);
    return true;
}

bool mesh_build_route_update(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_route_update_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t raw[6];

    if (payload == NULL) {
        return false;
    }
    if (payload->action != MESH_ROUTE_SET && payload->action != MESH_ROUTE_DELETE) {
        return false;
    }

    raw[0] = payload->dst;
    raw[1] = payload->next_hop;
    raw[2] = payload->metric;
    put_le16(raw + 3, payload->route_version);
    raw[5] = payload->action;

    return encode_payload_to_frame(
        MESH_TYPE_ROUTE_UPDATE,
        src,
        dst,
        seq,
        hop,
        (uint8_t)(MESH_FLAG_CONTROL | MESH_FLAG_NEEDS_ACK),
        raw,
        (uint16_t)sizeof(raw),
        out_frame,
        out_cap,
        out_len);
}

bool mesh_parse_route_update(
    const struct mesh_frame_view *frame,
    struct mesh_route_update_payload *out_payload)
{
    if (frame == NULL || out_payload == NULL) {
        return false;
    }
    if (frame->type != MESH_TYPE_ROUTE_UPDATE || frame->payload_len != 6u) {
        return false;
    }

    out_payload->dst = frame->payload[0];
    out_payload->next_hop = frame->payload[1];
    out_payload->metric = frame->payload[2];
    out_payload->route_version = get_le16(frame->payload + 3);
    out_payload->action = frame->payload[5];
    if (out_payload->action != MESH_ROUTE_SET && out_payload->action != MESH_ROUTE_DELETE) {
        return false;
    }
    return true;
}

bool mesh_build_link_state(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_link_state_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t raw[3];

    if (payload == NULL) {
        return false;
    }

    raw[0] = payload->neighbor;
    raw[1] = payload->link_up;
    raw[2] = payload->quality;
    return encode_payload_to_frame(
        MESH_TYPE_LINK_STATE,
        src,
        dst,
        seq,
        hop,
        MESH_FLAG_CONTROL,
        raw,
        (uint16_t)sizeof(raw),
        out_frame,
        out_cap,
        out_len);
}

bool mesh_parse_link_state(const struct mesh_frame_view *frame, struct mesh_link_state_payload *out_payload)
{
    if (frame == NULL || out_payload == NULL) {
        return false;
    }
    if (frame->type != MESH_TYPE_LINK_STATE || frame->payload_len != 3u) {
        return false;
    }

    out_payload->neighbor = frame->payload[0];
    out_payload->link_up = frame->payload[1];
    out_payload->quality = frame->payload[2];
    return true;
}

bool mesh_build_error(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_error_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint8_t raw[4];

    if (payload == NULL) {
        return false;
    }

    put_le16(raw, payload->code);
    put_le16(raw + 2, payload->related_seq);
    return encode_payload_to_frame(
        MESH_TYPE_ERROR,
        src,
        dst,
        seq,
        hop,
        MESH_FLAG_CONTROL,
        raw,
        (uint16_t)sizeof(raw),
        out_frame,
        out_cap,
        out_len);
}

bool mesh_parse_error(const struct mesh_frame_view *frame, struct mesh_error_payload *out_payload)
{
    if (frame == NULL || out_payload == NULL) {
        return false;
    }
    if (frame->type != MESH_TYPE_ERROR || frame->payload_len != 4u) {
        return false;
    }

    out_payload->code = get_le16(frame->payload);
    out_payload->related_seq = get_le16(frame->payload + 2);
    return true;
}

const char *mesh_error_name(uint16_t code)
{
    switch (code) {
        case MESH_ERR_BAD_FRAME:
            return "MESH_ERR_BAD_FRAME";
        case MESH_ERR_UNSUPPORTED_TYPE:
            return "MESH_ERR_UNSUPPORTED_TYPE";
        case MESH_ERR_NO_ROUTE:
            return "MESH_ERR_NO_ROUTE";
        case MESH_ERR_NOT_AUTHORIZED:
            return "MESH_ERR_NOT_AUTHORIZED";
        case MESH_ERR_INVALID_STATE:
            return "MESH_ERR_INVALID_STATE";
        case MESH_ERR_BUSY:
            return "MESH_ERR_BUSY";
        default:
            return "MESH_ERR_UNKNOWN";
    }
}
