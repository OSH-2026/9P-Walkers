/*
 * pwos_link_frame.c - PWOS 链路层帧格式编解码实现
 *
 * 本文件提供链路帧的编码（pwos_link_encode）、解码（pwos_link_decode）以及
 * 若干辅助工具函数（状态码转字符串、帧长度计算、类型判断）。
 *
 * 编码流程
 * --------
 *   1. 参数校验：输出缓冲区、payload 指针、长度是否合法。
 *   2. 按小端序填充固定头部字段（magic、version、hdr_len、type、flags、
 *      src、dst、ttl、seq、ack、payload_len）。
 *   3. 计算并写入 header CRC（覆盖 hdr_crc 之前的所有字节）。
 *   4. 计算并写入 payload CRC（只覆盖 payload）。
 *   5. 拷贝 payload 到头部之后。
 *   6. 返回实际帧长度。
 *
 * 解码流程
 * --------
 *   1. 参数校验。
 *   2. 检查最小长度、magic、version、hdr_len。
 *   3. 校验 header CRC。
 *   4. 读取 payload_len，检查是否越界，核对 frame_len 是否匹配。
 *   5. 校验 payload CRC。
 *   6. 填充 pwos_link_frame_view_t 并返回 PWOS_OK。
 */

#include "pwos_link_frame.h"

#include <string.h>

/*
 * 按小端序写入 16 位无符号整数。
 *
 * @param dst   目标缓冲区，至少 2 字节。
 * @param value 待写入的值。
 */
static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

/*
 * 按小端序读取 16 位无符号整数。
 *
 * @param src 源缓冲区，至少 2 字节。
 * @return    读取到的 16 位值。
 */
static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

/*
 * 把状态码转换为可读字符串。
 *
 * 主要用于日志打印和调试；未知状态统一返回 "E_UNKNOWN"。
 *
 * @param status 状态码。
 * @return       对应的常量字符串。
 */
const char *pwos_status_string(pwos_status_t status)
{
    switch (status) {
    case PWOS_OK:
        return "PWOS_OK";
    case PWOS_E_BAD_MAGIC:
        return "E_BAD_MAGIC";
    case PWOS_E_BAD_VERSION:
        return "E_BAD_VERSION";
    case PWOS_E_BAD_LENGTH:
        return "E_BAD_LENGTH";
    case PWOS_E_BAD_CRC:
        return "E_BAD_CRC";
    case PWOS_E_NO_MEMORY:
        return "E_NO_MEMORY";
    case PWOS_E_QUEUE_FULL:
        return "E_QUEUE_FULL";
    case PWOS_E_DEADLINE:
        return "E_DEADLINE";
    case PWOS_E_LINK_DOWN:
        return "E_LINK_DOWN";
    case PWOS_E_NO_ROUTE:
        return "E_NO_ROUTE";
    default:
        return "E_UNKNOWN";
    }
}

/*
 * 根据 payload 长度计算完整帧的线缆长度。
 *
 * 当前头部固定 19 字节，因此总长度 = 19 + payload_len。
 * 如果 payload_len 超过协议最大值，返回 0 表示非法。
 *
 * @param payload_len payload 长度。
 * @return            完整帧字节数；非法长度返回 0。
 */
size_t pwos_link_frame_wire_len(uint16_t payload_len)
{
    if (payload_len > PWOS_LINK_MAX_PAYLOAD_LEN) {
        return 0u;
    }
    return (size_t)PWOS_LINK_HDR_LEN + (size_t)payload_len;
}

/*
 * 判断帧类型是否属于链路维护类。
 *
 * 链路维护类型范围：0x01 ~ 0x04。
 *
 * @param type 帧类型字节。
 * @return     true 表示属于链路维护类型。
 */
bool pwos_link_type_is_link(uint8_t type)
{
    return type >= (uint8_t)PWOS_LINK_TYPE_LINK_HELLO &&
        type <= (uint8_t)PWOS_LINK_TYPE_LINK_ERROR;
}

/*
 * 判断帧类型是否属于控制面类。
 *
 * 控制面类型范围：0x10 ~ 0x1F。
 *
 * @param type 帧类型字节。
 * @return     true 表示属于控制面类型。
 */
bool pwos_link_type_is_control(uint8_t type)
{
    return type >= (uint8_t)PWOS_LINK_TYPE_CTRL_NODE_REGISTER &&
        type <= (uint8_t)PWOS_LINK_TYPE_CTRL_ERROR;
}

/*
 * 判断帧类型是否属于数据面类。
 *
 * 数据面类型范围：0x80 ~ 0x83。
 *
 * @param type 帧类型字节。
 * @return     true 表示属于数据面类型。
 */
bool pwos_link_type_is_data(uint8_t type)
{
    return type >= (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P &&
        type <= (uint8_t)PWOS_LINK_TYPE_DATA_BULK;
}

/*
 * 编码一帧 PWOS link frame。
 *
 * 输出缓冲区由调用方提供，函数不会分配内存。编码顺序严格遵循 v2 线缆格式。
 *
 * 关于 CRC：
 *   - header CRC 覆盖从 magic[0] 到 payload_len[1] 的 15 字节，不包括 hdr_crc
 *     自身。这样 parser 在收到 19 字节固定头部后即可判断头部是否损坏，不必等
 *     到 payload。
 *   - payload CRC 只覆盖 payload 数据；空 payload 时 CRC 为 0xFFFF（由
 *     pwos_link_crc16_ccitt_false(NULL, 0) 返回）。
 *
 * @param type        帧类型，见 pwos_link_type_t。
 * @param flags       标志位，当前协议版本预留为 0。
 * @param src         源 mesh 短地址。
 * @param dst         目的 mesh 短地址。
 * @param ttl         剩余跳数，转发时递减。
 * @param seq         发送序号。
 * @param ack         确认序号。
 * @param payload     payload 数据指针；payload_len 为 0 时可为 NULL。
 * @param payload_len payload 长度，必须 <= PWOS_LINK_MAX_PAYLOAD_LEN。
 * @param out_frame   输出帧缓冲区。
 * @param out_cap     输出缓冲区容量。
 * @param out_len     输出参数，返回实际写入的字节数。
 * @return            PWOS_OK 成功；PWOS_E_NO_MEMORY 输出缓冲区不足或必要参数为 NULL；
 *                    PWOS_E_BAD_LENGTH payload 长度非法或 payload 为空但长度非 0。
 */
pwos_status_t pwos_link_encode(
    uint8_t type,
    uint8_t flags,
    uint8_t src,
    uint8_t dst,
    uint8_t ttl,
    uint16_t seq,
    uint16_t ack,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len)
{
    uint16_t hdr_crc;
    uint16_t payload_crc;
    size_t total_len;

    /* 输出缓冲区和长度返回指针不能为空。 */
    if (out_frame == NULL || out_len == NULL) {
        return PWOS_E_NO_MEMORY;
    }
    /* payload_len 非 0 时 payload 指针必须有效。 */
    if (payload_len > 0u && payload == NULL) {
        return PWOS_E_BAD_LENGTH;
    }

    total_len = pwos_link_frame_wire_len(payload_len);
    if (total_len == 0u) {
        return PWOS_E_BAD_LENGTH;
    }
    if (out_cap < total_len) {
        return PWOS_E_NO_MEMORY;
    }

    /* 填充固定头部（magic ~ payload_len），全部使用小端序。 */
    out_frame[PWOS_LINK_OFF_MAGIC0] = PWOS_LINK_MAGIC0;
    out_frame[PWOS_LINK_OFF_MAGIC1] = PWOS_LINK_MAGIC1;
    out_frame[PWOS_LINK_OFF_VERSION] = PWOS_LINK_VERSION;
    out_frame[PWOS_LINK_OFF_HDR_LEN] = PWOS_LINK_HDR_LEN;
    out_frame[PWOS_LINK_OFF_TYPE] = type;
    out_frame[PWOS_LINK_OFF_FLAGS] = flags;
    out_frame[PWOS_LINK_OFF_SRC] = src;
    out_frame[PWOS_LINK_OFF_DST] = dst;
    out_frame[PWOS_LINK_OFF_TTL] = ttl;
    put_le16(out_frame + PWOS_LINK_OFF_SEQ, seq);
    put_le16(out_frame + PWOS_LINK_OFF_ACK, ack);
    put_le16(out_frame + PWOS_LINK_OFF_PAYLOAD_LEN, payload_len);

    /*
     * 计算 header CRC。
     *
     * CRC 范围是从 buffer[0] 到 hdr_crc 之前的一个字节，即
     * PWOS_LINK_OFF_HDR_CRC（15）字节。这样 header CRC 不包含自身，也不包含
     * payload_crc 和 payload。
     */
    hdr_crc = pwos_link_crc16_ccitt_false(out_frame, PWOS_LINK_OFF_HDR_CRC);
    put_le16(out_frame + PWOS_LINK_OFF_HDR_CRC, hdr_crc);

    /*
     * 计算 payload CRC。
     *
     * 空 payload 时 pwos_link_crc16_ccitt_false(NULL, 0) 返回 0xFFFF。
     */
    payload_crc = pwos_link_crc16_ccitt_false(payload, payload_len);
    put_le16(out_frame + PWOS_LINK_OFF_PAYLOAD_CRC, payload_crc);

    /* 拷贝 payload 到头部之后。 */
    if (payload_len > 0u) {
        memcpy(out_frame + PWOS_LINK_OFF_PAYLOAD, payload, payload_len);
    }

    *out_len = total_len;
    return PWOS_OK;
}

/*
 * 解码一帧 PWOS link frame。
 *
 * 这是一个严格的校验函数，任何字段不合法都会返回对应错误码，不会尝试容错。
 * 解码成功后，out_view 中的 payload 指针直接引用输入 frame 缓冲区的内存，
 * 调用方需要注意生命周期。
 *
 * 校验顺序：
 *   1. 参数非空。
 *   2. 长度至少为 PWOS_LINK_HDR_LEN。
 *   3. magic 前缀是否为 "MH"。
 *   4. version 是否为 PWOS_LINK_VERSION。
 *   5. hdr_len 是否为 PWOS_LINK_HDR_LEN。
 *   6. header CRC 是否匹配。
 *   7. payload_len 是否越界，frame_len 是否与期望总长度一致。
 *   8. payload CRC 是否匹配。
 *
 * @param frame     输入帧缓冲区。
 * @param frame_len 输入帧长度。
 * @param out_view  输出视图结构体。
 * @return          PWOS_OK 成功；否则返回对应错误码。
 */
pwos_status_t pwos_link_decode(
    const uint8_t *frame,
    size_t frame_len,
    pwos_link_frame_view_t *out_view)
{
    uint16_t payload_len;
    uint16_t actual_hdr_crc;
    uint16_t expected_hdr_crc;
    uint16_t actual_payload_crc;
    uint16_t expected_payload_crc;
    size_t expected_len;

    /* 参数校验。 */
    if (frame == NULL || out_view == NULL) {
        return PWOS_E_NO_MEMORY;
    }
    if (frame_len < PWOS_LINK_HDR_LEN) {
        return PWOS_E_BAD_LENGTH;
    }

    /* 校验 magic、version、hdr_len。 */
    if (frame[PWOS_LINK_OFF_MAGIC0] != PWOS_LINK_MAGIC0 ||
        frame[PWOS_LINK_OFF_MAGIC1] != PWOS_LINK_MAGIC1) {
        return PWOS_E_BAD_MAGIC;
    }
    if (frame[PWOS_LINK_OFF_VERSION] != PWOS_LINK_VERSION) {
        return PWOS_E_BAD_VERSION;
    }
    if (frame[PWOS_LINK_OFF_HDR_LEN] != PWOS_LINK_HDR_LEN) {
        return PWOS_E_BAD_LENGTH;
    }

    /* 校验 header CRC。 */
    actual_hdr_crc = get_le16(frame + PWOS_LINK_OFF_HDR_CRC);
    expected_hdr_crc = pwos_link_crc16_ccitt_false(frame, PWOS_LINK_OFF_HDR_CRC);
    if (actual_hdr_crc != expected_hdr_crc) {
        return PWOS_E_BAD_CRC;
    }

    /* 读取并校验 payload 长度。 */
    payload_len = get_le16(frame + PWOS_LINK_OFF_PAYLOAD_LEN);
    if (payload_len > PWOS_LINK_MAX_PAYLOAD_LEN) {
        return PWOS_E_BAD_LENGTH;
    }

    /* 核对实际帧长度是否与期望长度一致。 */
    expected_len = pwos_link_frame_wire_len(payload_len);
    if (frame_len != expected_len) {
        return PWOS_E_BAD_LENGTH;
    }

    /* 校验 payload CRC。 */
    actual_payload_crc = get_le16(frame + PWOS_LINK_OFF_PAYLOAD_CRC);
    expected_payload_crc = pwos_link_crc16_ccitt_false(
        frame + PWOS_LINK_OFF_PAYLOAD,
        payload_len);
    if (actual_payload_crc != expected_payload_crc) {
        return PWOS_E_BAD_CRC;
    }

    /* 所有校验通过，填充视图结构体。 */
    out_view->version = frame[PWOS_LINK_OFF_VERSION];
    out_view->type = frame[PWOS_LINK_OFF_TYPE];
    out_view->flags = frame[PWOS_LINK_OFF_FLAGS];
    out_view->src = frame[PWOS_LINK_OFF_SRC];
    out_view->dst = frame[PWOS_LINK_OFF_DST];
    out_view->ttl = frame[PWOS_LINK_OFF_TTL];
    out_view->seq = get_le16(frame + PWOS_LINK_OFF_SEQ);
    out_view->ack = get_le16(frame + PWOS_LINK_OFF_ACK);
    out_view->payload = frame + PWOS_LINK_OFF_PAYLOAD;
    out_view->payload_len = payload_len;

    return PWOS_OK;
}
