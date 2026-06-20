/*
 * pwos_link_parser.c - PWOS 链路层字节流解析器实现
 *
 * 状态机说明
 * ----------
 * 解析器采用线性缓冲区 + 状态隐含在 len / want_len 中的设计：
 *   - len == 0：尚未找到帧头，正在寻找 magic[0]。
 *   - len == 1：已收到 magic[0]，等待 magic[1]。
 *   - 2 <= len < PWOS_LINK_HDR_LEN：正在接收头部固定字段。
 *   - len == PWOS_LINK_HDR_LEN 且 want_len 已计算：头部 CRC、版本、长度已验证，
 *     正在接收 payload。
 *   - len == want_len：整帧接收完成，调用 pwos_link_decode 做最终校验与解码。
 *
 * 重新同步策略
 * ------------
 *   1. 未锁定帧头时只接受 'M' 'H'；噪声字节静默丢弃，避免日志被随机噪声淹没。
 *   2. 锁定帧头后即使 payload 里出现 'M' 'H' 也不重新同步，防止把真实 payload
 *      误当作新帧头。
 *   3. 头部阶段出错时尽量保留尾部 magic 前缀（0、1 或 2 字节），使“坏帧后紧跟好帧”
 *      的场景能快速恢复。
 *   4. payload 阶段出错（解码失败）时直接丢弃整个候选帧，不保留前缀，因为此时
 *      已经不知道边界在哪里。
 */

#include "pwos_link_parser.h"

#include <string.h>

/*
 * 按小端序读取 2 字节无符号整数。
 *
 * 链路帧所有多字节字段均使用 little-endian，本函数不依赖 CPU 字节序。
 *
 * @param src 指向至少包含 2 字节有效数据的缓冲区。
 * @return    16 位无符号整数。
 */
static uint16_t parser_get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

/*
 * 清空事件结构体。
 *
 * 把所有字段置 0，并把 kind 设为 PWOS_LINK_PARSE_NONE，status 设为 PWOS_OK。
 * 在生成新事件前调用，避免调用方看到残留数据。
 *
 * @param event 待清空的事件。为 NULL 时安全返回。
 */
static void clear_event(pwos_link_parse_event_t *event)
{
    if (event == NULL) {
        return;
    }
    memset(event, 0, sizeof(*event));
    event->kind = PWOS_LINK_PARSE_NONE;
    event->status = PWOS_OK;
}

/*
 * 彻底丢弃当前候选帧。
 *
 * 把 len 和 want_len 都清零，但不清空 buffer 内容（后续会覆盖）。
 *
 * @param parser 解析器实例。
 */
static void reset_candidate(pwos_link_parser_t *parser)
{
    parser->len = 0u;
    parser->want_len = 0u;
}

/*
 * 头部阶段出错后，保留尾部可能属于下一帧的 magic 前缀。
 *
 * 错误候选帧的末尾可能恰好是一个新帧的开头。为了快速恢复同步，本函数检查
 * buffer 尾部是否有 'M'、'MH' 前缀，并保留下来：
 *   - 末尾为 "MH"：保留 2 字节，len=2，后续可直接进入头部接收。
 *   - 末尾为 "M"：保留 1 字节，len=1，后续等待 'H'。
 *   - 其他：完全丢弃，回到 len=0 状态。
 *
 * 该逻辑简单且覆盖了最常见的“坏帧后紧跟好帧”场景。注意：它只检查最后 1~2 字节，
 * 不会向前回溯更多，因为那样会显著增加 ISR 中的处理时间。
 *
 * @param parser 解析器实例。
 */
static void keep_trailing_magic_prefix(pwos_link_parser_t *parser)
{
    if (parser->len >= 2u &&
        parser->buffer[parser->len - 2u] == PWOS_LINK_MAGIC0 &&
        parser->buffer[parser->len - 1u] == PWOS_LINK_MAGIC1) {
        parser->buffer[0] = PWOS_LINK_MAGIC0;
        parser->buffer[1] = PWOS_LINK_MAGIC1;
        parser->len = 2u;
        parser->want_len = 0u;
        return;
    }

    if (parser->len >= 1u &&
        parser->buffer[parser->len - 1u] == PWOS_LINK_MAGIC0) {
        parser->buffer[0] = PWOS_LINK_MAGIC0;
        parser->len = 1u;
        parser->want_len = 0u;
        return;
    }

    reset_candidate(parser);
}

/*
 * 统一输出 ERROR 事件并重置/保留候选帧。
 *
 * @param parser            解析器实例。
 * @param event             输出事件。非 NULL 时填充为 ERROR 并写入 status。
 * @param status            错误状态码。
 * @param keep_magic_prefix 非 0 时调用 keep_trailing_magic_prefix 保留 magic 前缀；
 *                          为 0 时直接 reset_candidate。
 * @return                  固定返回 PWOS_LINK_PARSE_ERROR。
 */
static pwos_link_parse_kind_t emit_error(
    pwos_link_parser_t *parser,
    pwos_link_parse_event_t *event,
    pwos_status_t status,
    int keep_magic_prefix)
{
    ++parser->errors;
    if (event != NULL) {
        event->kind = PWOS_LINK_PARSE_ERROR;
        event->status = status;
    }

    if (keep_magic_prefix) {
        keep_trailing_magic_prefix(parser);
    } else {
        reset_candidate(parser);
    }

    return PWOS_LINK_PARSE_ERROR;
}

/*
 * 初始化解析器。
 *
 * 参见头文件中的完整说明。本实现把 parser 全部字段清零，包括统计计数器。
 */
void pwos_link_parser_init(pwos_link_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
}

/*
 * 重置解析器。
 *
 * 仅丢弃当前候选帧（len/want_len），不清空统计计数器。
 */
void pwos_link_parser_reset(pwos_link_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    reset_candidate(parser);
}

/*
 * 单字节状态机核心实现。
 *
 * 流程概述：
 *   1. 参数校验与事件清零。
 *   2. 未锁定帧头时寻找 magic。
 *   3. 已锁定帧头后逐字节存入缓冲区。
 *   4. 在 PWOS_LINK_OFF_TYPE 位置（即第 5 字节，索引 4）刚写入时，
 *      检查 version 和 hdr_len 是否合法。
 *   5. 头部收齐后校验 hdr_crc，计算 payload_len，得到 want_len。
 *   6. 当 len == want_len 时调用 pwos_link_decode 解码并输出事件。
 */
pwos_link_parse_kind_t pwos_link_parser_feed_byte(
    pwos_link_parser_t *parser,
    uint8_t byte,
    pwos_link_parse_event_t *event)
{
    uint16_t hdr_crc;
    uint16_t expected_hdr_crc;
    uint16_t payload_len;
    pwos_status_t status;

    /* parser 或 event 为 NULL 时无法继续，直接返回错误。 */
    if (parser == NULL || event == NULL) {
        if (event != NULL) {
            clear_event(event);
            event->kind = PWOS_LINK_PARSE_ERROR;
            event->status = PWOS_E_NO_MEMORY;
        }
        return PWOS_LINK_PARSE_ERROR;
    }

    clear_event(event);
    ++parser->bytes_seen;

    /*
     * 状态 0：尚未收到任何 magic 前缀。
     *
     * 随机噪声字节直接丢弃，不产生错误事件，防止上层日志被刷爆。
     * 一旦收到 magic[0]，进入状态 1。
     */
    if (parser->len == 0u) {
        if (byte == PWOS_LINK_MAGIC0) {
            parser->buffer[0] = byte;
            parser->len = 1u;
        }
        return PWOS_LINK_PARSE_NONE;
    }

    /*
     * 状态 1：已收到 magic[0]，等待 magic[1]。
     *
     * 特殊处理：如果此时又来一个 magic[0]，保留它作为新的候选帧开头。
     * 这能处理 "M M H ..." 这类噪声场景。
     */
    if (parser->len == 1u) {
        if (byte == PWOS_LINK_MAGIC1) {
            parser->buffer[1] = byte;
            parser->len = 2u;
        } else if (byte == PWOS_LINK_MAGIC0) {
            parser->buffer[0] = byte;
            parser->len = 1u;
        } else {
            reset_candidate(parser);
        }
        return PWOS_LINK_PARSE_NONE;
    }

    /*
     * 状态 >=2：已锁定候选帧。把新字节追加到缓冲区。
     *
     * 如果缓冲区已满（理论上不应发生，因为 want_len 会在头部收齐后立即确定），
     * 按长度错误处理，不保留 magic 前缀。
     */
    if (parser->len >= sizeof(parser->buffer)) {
        return emit_error(parser, event, PWOS_E_BAD_LENGTH, 0);
    }

    parser->buffer[parser->len] = byte;
    ++parser->len;

    /*
     * 第 5 字节（type 字段）写入完成时，已经能看到 version 和 hdr_len。
     *
     * 这里提前做基础校验，避免继续接收明显错误的帧。
     * 注意：此时只接收了 type 字段，payload_len 还未收齐，hdr_crc 也未校验。
     */
    if (parser->len == (size_t)PWOS_LINK_OFF_TYPE) {
        /* 版本号不匹配：保留 magic 前缀以便快速恢复。 */
        if (parser->buffer[PWOS_LINK_OFF_VERSION] != PWOS_LINK_VERSION) {
            return emit_error(parser, event, PWOS_E_BAD_VERSION, 1);
        }
        /* 头部长度不是预期值：保留 magic 前缀。 */
        if (parser->buffer[PWOS_LINK_OFF_HDR_LEN] != PWOS_LINK_HDR_LEN) {
            return emit_error(parser, event, PWOS_E_BAD_LENGTH, 1);
        }
    }

    /*
     * 头部收齐且尚未计算 want_len 时：校验头部 CRC 并推导完整帧长度。
     *
     * 头部 CRC 覆盖 magic[0] 到 payload_len[1]，不包括 hdr_crc 本身。
     * 通过后再读取 payload_len，并检查是否超过协议最大值。
     * want_len = 19 字节头部 + payload_len（payload_crc 已包含在头部中，
     * 具体见 pwos_link_frame_wire_len）。
     */
    if (parser->len == PWOS_LINK_HDR_LEN && parser->want_len == 0u) {
        hdr_crc = parser_get_le16(parser->buffer + PWOS_LINK_OFF_HDR_CRC);
        expected_hdr_crc = pwos_link_crc16_ccitt_false(
            parser->buffer,
            PWOS_LINK_OFF_HDR_CRC);
        if (hdr_crc != expected_hdr_crc) {
            return emit_error(parser, event, PWOS_E_BAD_CRC, 1);
        }

        payload_len = parser_get_le16(parser->buffer + PWOS_LINK_OFF_PAYLOAD_LEN);
        if (payload_len > PWOS_LINK_MAX_PAYLOAD_LEN) {
            return emit_error(parser, event, PWOS_E_BAD_LENGTH, 1);
        }

        parser->want_len = pwos_link_frame_wire_len(payload_len);
    }

    /*
     * 已计算出完整帧长度且缓冲区已收齐：进行最终解码。
     *
     * pwos_link_decode 会再次检查 magic、version、hdr_len、payload_crc 等。
     * 解码成功则输出 FRAME 事件并清空候选帧；失败则输出 ERROR 事件并丢弃候选帧
     *（payload 阶段不再保留 magic 前缀）。
     */
    if (parser->want_len != 0u && parser->len == parser->want_len) {
        status = pwos_link_decode(parser->buffer, parser->len, &event->frame);
        event->status = status;
        event->raw_frame = parser->buffer;
        event->raw_frame_len = parser->len;

        if (status == PWOS_OK) {
            ++parser->frames_ok;
            event->kind = PWOS_LINK_PARSE_FRAME;
            reset_candidate(parser);
            return PWOS_LINK_PARSE_FRAME;
        }

        return emit_error(parser, event, status, 0);
    }

    /* 帧尚未收齐，等待更多字节。 */
    return PWOS_LINK_PARSE_NONE;
}

/*
 * 批量喂入字节的便捷封装。
 *
 * 内部逐字节调用 pwos_link_parser_feed_byte，直到：
 *   - 产生 FRAME 或 ERROR 事件，返回该事件并把 consumed 设为已消费字节数；
 *   - 或全部处理完毕仍未产生事件，返回 NONE 并把 consumed 设为 len。
 *
 * 调用方应检查 consumed，从 data + consumed 继续喂入剩余字节。
 */
pwos_link_parse_kind_t pwos_link_parser_feed(
    pwos_link_parser_t *parser,
    const uint8_t *data,
    size_t len,
    pwos_link_parse_event_t *event,
    size_t *consumed)
{
    size_t i;
    pwos_link_parse_kind_t kind = PWOS_LINK_PARSE_NONE;

    if (consumed != NULL) {
        *consumed = 0u;
    }
    /*
     * 参数校验。
     * data 为 NULL 但 len > 0 属于无效输入；如果 event 可用，则标记为错误事件。
     * parser 或 event 为 NULL 时也返回 ERROR。
     */
    if (parser == NULL || event == NULL || (data == NULL && len > 0u)) {
        if (event != NULL) {
            clear_event(event);
            event->kind = PWOS_LINK_PARSE_ERROR;
            event->status = PWOS_E_NO_MEMORY;
        }
        return PWOS_LINK_PARSE_ERROR;
    }

    clear_event(event);
    for (i = 0u; i < len; ++i) {
        kind = pwos_link_parser_feed_byte(parser, data[i], event);
        if (kind != PWOS_LINK_PARSE_NONE) {
            if (consumed != NULL) {
                *consumed = i + 1u;
            }
            return kind;
        }
    }

    if (consumed != NULL) {
        *consumed = len;
    }
    return PWOS_LINK_PARSE_NONE;
}
