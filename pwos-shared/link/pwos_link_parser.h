#ifndef PWOS_LINK_PARSER_H
#define PWOS_LINK_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_link_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// 解析状态机：NONE 表示未产生事件，FRAME 表示成功解析出一帧，ERROR 表示解析出错。
typedef enum {
    PWOS_LINK_PARSE_NONE = 0,
    PWOS_LINK_PARSE_FRAME = 1,
    PWOS_LINK_PARSE_ERROR = 2
} pwos_link_parse_kind_t;

// 解析出的事件的结构体
// 类型、状态、帧视图、原始帧指针和长度
typedef struct {
    pwos_link_parse_kind_t kind;
    pwos_status_t status;
    pwos_link_frame_view_t frame;
    const uint8_t *raw_frame;
    size_t raw_frame_len;
} pwos_link_parse_event_t;

// 链路层的字节流解析器状态结构体，作用是把从 UART/USB/RDMA 等链路收到的 无结构字节流 还原成完整的 pwos_link_frame_view_t 帧
typedef struct {
    uint8_t buffer[PWOS_LINK_MAX_FRAME_LEN];
    size_t len;
    size_t want_len;
    uint32_t frames_ok;
    uint32_t errors;
    uint32_t bytes_seen;
} pwos_link_parser_t;

/*
 * 初始化/重置 parser。
 *
 * parser 没有动态内存，也没有阻塞等待；ISR 或任务层可以逐字节、按 DMA 块
 * 或按任意切片喂入。注意：如果在 ISR 中使用，完成帧后的入队动作仍应由
 * 调用方控制，本 parser 本身不碰队列和 RTOS API。
 */
void pwos_link_parser_init(pwos_link_parser_t *parser);
void pwos_link_parser_reset(pwos_link_parser_t *parser);

/*
 * 喂入一个字节。
 *
 * 重新同步策略：
 * - 未锁定帧头时只寻找 'M' 'H'。
 * - 一旦头部 CRC 通过并进入 payload 阶段，即使 payload 内出现 'M' 'H'
 *   也不会重新同步，避免误切真实数据。
 * - 长度、版本或 CRC 错误只丢弃当前候选帧；后续字节可继续恢复。
 */
pwos_link_parse_kind_t pwos_link_parser_feed_byte(
    pwos_link_parser_t *parser,
    uint8_t byte,
    pwos_link_parse_event_t *event);

/*
 * 喂入一段字节，最多返回一个事件。若返回 FRAME/ERROR，consumed 表示已经
 * 消费到事件为止的字节数；调用方可从 data + consumed 继续喂剩余字节。
 */
pwos_link_parse_kind_t pwos_link_parser_feed(
    pwos_link_parser_t *parser,
    const uint8_t *data,
    size_t len,
    pwos_link_parse_event_t *event,
    size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif
