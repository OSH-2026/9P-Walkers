/*
 * pwos_link_frame.h - PWOS 链路层帧格式定义
 *
 * 本文件定义 PWOS link frame v2 的线缆格式、常量、类型枚举以及编解码接口。
 *
 * 设计原则
 * --------
 *   - 与上层解耦：本层只负责链路帧格式，不理解 mini9P、VFS、路由表等内容，
 *     也不依赖具体的 HAL/IDF。
 *   - 小端序：所有多字节字段均使用 little-endian。
 *   - 双层 CRC：头部 CRC 覆盖固定头部，payload CRC 只覆盖 payload。
 *     这样可以在收齐 19 字节头部后立即诊断头部错误，不必等待整个帧。
 *   - 固定头部：当前头部固定 19 字节，未来通过 hdr_len 字段兼容扩展。
 *
 * 线缆格式（总字节数 = 19 + payload_len）
 * -------------------------------------------
 *   offset  size  字段名          说明
 *   0       2B    magic           固定 'M' 'H'
 *   2       1B    version         当前为 2
 *   3       1B    hdr_len         当前固定为 19，未来扩展头部时靠它兼容
 *   4       1B    type            LINK_/CTRL_/DATA_ 类型
 *   5       1B    flags           分片、压缩、加密等预留位
 *   6       1B    src             源 mesh 短地址
 *   7       1B    dst             目的 mesh 短地址，0xFF 可表示未分配/广播
 *   8       1B    ttl             多跳防环，每转发一次递减
 *   9       2B    seq             发送序号，little-endian
 *   11      2B    ack             确认序号，第一版可不启用
 *   13      2B    payload_len     payload 长度，最大 512
 *   15      2B    hdr_crc         覆盖 magic..payload_len，不覆盖 hdr_crc 自己
 *   17      2B    payload_crc     只覆盖 payload；空 payload 的 CRC 为 0xFFFF
 *   19      N     payload         上层数据
 *
 * 基本用法
 * --------
 * 编码：
 *     uint8_t frame[256];
 *     size_t frame_len;
 *     pwos_status_t st = pwos_link_encode(
 *         PWOS_LINK_TYPE_DATA_MINI9P, 0, src, dst, ttl, seq, 0,
 *         payload, payload_len,
 *         frame, sizeof(frame), &frame_len);
 *
 * 解码：
 *     pwos_link_frame_view_t view;
 *     pwos_status_t st = pwos_link_decode(frame, frame_len, &view);
 *     if (st == PWOS_OK) {
 *         // view.payload / view.payload_len 指向 payload
 *     }
 */

#ifndef PWOS_LINK_FRAME_H
#define PWOS_LINK_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pwos_link_crc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 帧 magic 前缀：固定为 'M' 'H'。
 *
 * 这两个字节用于 parser 在字节流中快速定位候选帧头。
 */
#define PWOS_LINK_MAGIC0 ((uint8_t)'M')
#define PWOS_LINK_MAGIC1 ((uint8_t)'H')

/*
 * 协议版本号与固定头部长度。
 *
 * PWOS_LINK_VERSION：当前链路帧协议版本为 2。
 * PWOS_LINK_HDR_LEN：当前固定头部为 19 字节。
 *                    未来扩展时，新字段加在 hdr_crc 之前，并通过 hdr_len
 *                    告知对端实际头部长度，实现前向兼容。
 */
#define PWOS_LINK_VERSION 2u
#define PWOS_LINK_HDR_LEN 19u

/*
 * payload 与帧大小限制。
 *
 * PWOS_LINK_MAX_PAYLOAD_LEN：单帧 payload 最大 512 字节。
 * PWOS_LINK_MAX_FRAME_LEN：  单帧总长度最大 531 字节（19 + 512）。
 */
#define PWOS_LINK_MAX_PAYLOAD_LEN 512u
#define PWOS_LINK_MAX_FRAME_LEN (PWOS_LINK_HDR_LEN + PWOS_LINK_MAX_PAYLOAD_LEN)

/*
 * 特殊 mesh 短地址。
 *
 * PWOS_LINK_ADDR_HOST：       主机/根节点地址，固定为 0x00。
 * PWOS_LINK_ADDR_UNASSIGNED： 未分配地址，可用于广播或初始注册阶段，值为 0xFF。
 */
#define PWOS_LINK_ADDR_HOST 0x00u
#define PWOS_LINK_ADDR_UNASSIGNED 0xFFu

/*
 * 帧内各字段的字节偏移量。
 *
 * 这些常量用于按索引访问缓冲区中的字段，避免硬编码数字。
 */
enum {
    PWOS_LINK_OFF_MAGIC0 = 0,
    PWOS_LINK_OFF_MAGIC1 = 1,
    PWOS_LINK_OFF_VERSION = 2,
    PWOS_LINK_OFF_HDR_LEN = 3,
    PWOS_LINK_OFF_TYPE = 4,
    PWOS_LINK_OFF_FLAGS = 5,
    PWOS_LINK_OFF_SRC = 6,
    PWOS_LINK_OFF_DST = 7,
    PWOS_LINK_OFF_TTL = 8,
    PWOS_LINK_OFF_SEQ = 9,
    PWOS_LINK_OFF_ACK = 11,
    PWOS_LINK_OFF_PAYLOAD_LEN = 13,
    PWOS_LINK_OFF_HDR_CRC = 15,
    PWOS_LINK_OFF_PAYLOAD_CRC = 17,
    PWOS_LINK_OFF_PAYLOAD = 19
};

/*
 * 通用状态码枚举。
 *
 * 这些状态码不仅用于链路层，也被上层模块复用。
 */
typedef enum {
    PWOS_OK = 0,                 /* 成功 */
    PWOS_E_BAD_MAGIC = -1,       /* magic 不匹配 */
    PWOS_E_BAD_VERSION = -2,     /* 协议版本不匹配 */
    PWOS_E_BAD_LENGTH = -3,      /* 长度非法或越界 */
    PWOS_E_BAD_CRC = -4,         /* CRC 校验失败 */
    PWOS_E_NO_MEMORY = -5,       /* 内存/缓冲区不足 */
    PWOS_E_QUEUE_FULL = -6,      /* 队列满 */
    PWOS_E_DEADLINE = -7,        /* 超时 */
    PWOS_E_LINK_DOWN = -8,       /* 链路断开 */
    PWOS_E_NO_ROUTE = -9         /* 无路由 */
} pwos_status_t;

/*
 * 链路帧类型枚举。
 *
 * 按功能分为三类：
 *   - 0x01~0x0F：链路维护类（hello、心跳、错误）。
 *   - 0x10~0x1F：控制面类（注册、地址分配、租约、路由等）。
 *   - 0x80~0x8F：数据面类（mini9P、RPC、JOB、BULK 等）。
 *
 * 判断类型所属类别可使用 pwos_link_type_is_link / is_control / is_data。
 */
typedef enum {
    /* 链路维护 */
    PWOS_LINK_TYPE_LINK_HELLO = 0x01,
    PWOS_LINK_TYPE_LINK_HELLO_ACK = 0x02,
    PWOS_LINK_TYPE_LINK_HEARTBEAT = 0x03,
    PWOS_LINK_TYPE_LINK_ERROR = 0x04,

    /* 控制面 */
    PWOS_LINK_TYPE_CTRL_NODE_REGISTER = 0x10,
    PWOS_LINK_TYPE_CTRL_ADDR_ASSIGN = 0x11,
    PWOS_LINK_TYPE_CTRL_LEASE_RENEW = 0x12,
    PWOS_LINK_TYPE_CTRL_LEASE_ACK = 0x13,
    PWOS_LINK_TYPE_CTRL_LINK_STATE = 0x14,
    PWOS_LINK_TYPE_CTRL_ROUTE_UPDATE = 0x15,
    PWOS_LINK_TYPE_CTRL_HOST_ADVERTISE = 0x16,
    PWOS_LINK_TYPE_CTRL_TIME_SYNC = 0x17,
    PWOS_LINK_TYPE_CTRL_ERROR = 0x1F,

    /* 数据面 */
    PWOS_LINK_TYPE_DATA_MINI9P = 0x80,
    PWOS_LINK_TYPE_DATA_RPC = 0x81,
    PWOS_LINK_TYPE_DATA_JOB = 0x82,
    PWOS_LINK_TYPE_DATA_BULK = 0x83
} pwos_link_type_t;

/*
 * 解码后的只读帧视图。
 *
 * 该结构体不包含 payload 副本，所有指针均引用外部传入的帧缓冲区。
 * 如果视图来自 parser，则下一次 feed 可能覆盖 payload 指向的内存，
 * 调用方需要立即处理或复制出去。
 */
typedef struct {
    uint8_t version;        /* 协议版本号 */
    uint8_t type;           /* 帧类型，见 pwos_link_type_t */
    uint8_t flags;          /* 标志位（分片、压缩、加密等预留） */
    uint8_t src;            /* 源地址 */
    uint8_t dst;            /* 目的地址 */
    uint8_t ttl;            /* 剩余跳数 */
    uint16_t seq;           /* 发送序号 */
    uint16_t ack;           /* 确认序号 */
    const uint8_t *payload; /* payload 指针，引用外部缓冲区 */
    uint16_t payload_len;   /* payload 长度 */
} pwos_link_frame_view_t;

/*
 * 把状态码转换为可读的字符串常量。
 *
 * @param status 状态码。
 * @return       对应的字符串，未知状态返回 "E_UNKNOWN"。
 */
const char *pwos_status_string(pwos_status_t status);

/*
 * 根据 payload 长度计算完整帧的线缆长度。
 *
 * @param payload_len payload 长度。
 * @return            完整帧字节数；payload_len 超过最大值时返回 0。
 */
size_t pwos_link_frame_wire_len(uint16_t payload_len);

/*
 * 判断帧类型是否属于链路维护类。
 *
 * @param type 帧类型字节。
 * @return     true 表示 type 在 0x01~0x04 范围内。
 */
bool pwos_link_type_is_link(uint8_t type);

/*
 * 判断帧类型是否属于控制面类。
 *
 * @param type 帧类型字节。
 * @return     true 表示 type 在 0x10~0x1F 范围内。
 */
bool pwos_link_type_is_control(uint8_t type);

/*
 * 判断帧类型是否属于数据面类。
 *
 * @param type 帧类型字节。
 * @return     true 表示 type 在 0x80~0x83 范围内。
 */
bool pwos_link_type_is_data(uint8_t type);

/*
 * 编码一帧 PWOS link frame。
 *
 * 调用方提供输出缓冲区，函数负责填充头部、计算 CRC、拷贝 payload。
 *
 * @param type        帧类型。
 * @param flags       标志位。
 * @param src         源地址。
 * @param dst         目的地址。
 * @param ttl         剩余跳数。
 * @param seq         发送序号。
 * @param ack         确认序号。
 * @param payload     payload 数据指针；payload_len 为 0 时可为 NULL。
 * @param payload_len payload 长度，必须 <= PWOS_LINK_MAX_PAYLOAD_LEN。
 * @param out_frame   输出帧缓冲区。
 * @param out_cap     输出缓冲区容量。
 * @param out_len     输出参数，返回实际写入的字节数。
 * @return            PWOS_OK 成功；PWOS_E_NO_MEMORY 输出缓冲区不足或参数为 NULL；
 *                    PWOS_E_BAD_LENGTH payload 长度非法。
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
    size_t *out_len);

/*
 * 解码一帧 PWOS link frame。
 *
 * 对输入缓冲区做完整性、magic、version、hdr_len、头部 CRC、payload CRC 校验，
 * 成功后把字段填充到 out_view。
 *
 * @param frame     输入帧缓冲区。
 * @param frame_len 输入帧长度。
 * @param out_view  输出视图结构体。
 * @return          PWOS_OK 成功；否则返回对应的错误码。
 */
pwos_status_t pwos_link_decode(
    const uint8_t *frame,
    size_t frame_len,
    pwos_link_frame_view_t *out_view);

#ifdef __cplusplus
}
#endif

#endif
