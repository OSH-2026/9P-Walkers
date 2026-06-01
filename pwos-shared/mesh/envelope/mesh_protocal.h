#ifndef MESH_PROTOCAL_H
#define MESH_PROTOCAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Mesh Envelope 协议（v1）
 *
 * 职责边界：
 * 1. 承载 mini9P 数据面帧（不改变 mini9P 语义）。
 * 2. 承载控制面消息（注册、分配、探活、路由更新等）。
 * 3. 让中继节点只看信封头进行转发，不解析上层文件协议。
 *
 * 编码约定：
 * - 所有多字节整数字段均为小端序。
 * - 字符串均采用“长度前缀 + 字节串”，线缆上不带 '\0'。
 */

/* 当前协议版本号。 */
#define MESH_VERSION 0x01u

/* 主机 mesh 短地址。 */
#define MESH_ADDR_HOST 0x00u

/* 未分配短地址，常用于新节点 bootstrap 注册阶段。 */
#define MESH_ADDR_UNASSIGNED 0xFFu

/* 设备 UID 固定字节数。 */
#define MESH_UID_LEN 8u

/* 节点名最大长度（不含结尾 '\0'）。 */
#define MESH_MAX_NODE_NAME 31u

/* 单帧最大 payload 长度。 */
#define MESH_MAX_PAYLOAD_LEN 512u

/* REGISTER.port_bitmap 的最高位预留给 Wi-Fi 传输。 */
#define MESH_PORT_SELECTOR_WIFI_ID 7u
#define MESH_PORT_SELECTOR_WIFI_MASK ((uint8_t)(1u << MESH_PORT_SELECTOR_WIFI_ID))

/* 最小整帧长度：Magic(2)+FrameLen(2)+固定头(8)+CRC(2)=14。 */
#define MESH_FRAME_OVERHEAD 14u

/*
 * 帧头 flags 位定义：
 * - NEEDS_ACK: 该帧在上层语义上需要确认。
 * - IS_RETRY: 该帧为重传帧。
 * - CONTROL: 该帧属于控制面。
 */
#define MESH_FLAG_NEEDS_ACK 0x01u
#define MESH_FLAG_IS_RETRY 0x02u
#define MESH_FLAG_CONTROL 0x04u

/*
 * 消息类型：
 * - MESH_TYPE_MINI9P: 数据面，payload 为完整 mini9P 帧。
 * - 其余类型：控制面。
 */
enum mesh_type {
    MESH_TYPE_MINI9P = 0x01,

    MESH_TYPE_REGISTER = 0x10,
    MESH_TYPE_ASSIGN = 0x11,
    MESH_TYPE_PING = 0x12,
    MESH_TYPE_PONG = 0x13,
    MESH_TYPE_TIME_SYNC = 0x14,
    MESH_TYPE_ROUTE_UPDATE = 0x15,
    MESH_TYPE_LINK_STATE = 0x16,
    MESH_TYPE_ERROR = 0x7F
};

/* 路由更新动作。 */
enum mesh_route_action {
    MESH_ROUTE_SET = 1,
    MESH_ROUTE_DELETE = 2
};

/* 控制面错误码。 */
enum mesh_error_code {
    MESH_ERR_BAD_FRAME = 0x0001,
    MESH_ERR_UNSUPPORTED_TYPE = 0x0002,
    MESH_ERR_NO_ROUTE = 0x0003,
    MESH_ERR_NOT_AUTHORIZED = 0x0004,
    MESH_ERR_INVALID_STATE = 0x0005,
    MESH_ERR_BUSY = 0x0006
};

/*
 * 线缆帧格式：
 * | Magic(2) | FrameLen(2) | Version(1) | Type(1) | Src(1) | Dst(1) |
 * | Seq(2) | Hop(1) | Flags(1) | Payload(N) | CRC16(2) |
 *
 * 说明：
 * - FrameLen = Version 到 Payload 末尾的总字节数。
 * - Seq 用于重传与去重相关匹配。
 * - Hop 为剩余跳数（类似 TTL），每次中继转发应递减。
 * - CRC16 覆盖 Version..Payload。
 */

/*
 * 解码后的只读帧视图。
 * payload 指针引用输入缓冲区中的内存，调用方需保证该缓冲区生命周期。
 */
struct mesh_frame_view {
    uint8_t version;
    uint8_t type;
    uint8_t src;
    uint8_t dst;
    uint16_t seq;
    uint8_t hop;
    uint8_t flags;
    const uint8_t *payload;
    uint16_t payload_len;
};

/* REGISTER：新节点上电后上报 UID、能力和端口信息。 */
struct mesh_register_payload {
    uint8_t uid[MESH_UID_LEN];
    uint32_t boot_nonce;
    uint16_t capability_bits;
    uint8_t port_bitmap;
    bool wifi_supported;
};

/* ASSIGN：主机下发节点地址、节点名及租约信息。 */
struct mesh_assign_payload {
    uint8_t uid[MESH_UID_LEN];
    uint8_t node_addr;
    uint32_t lease_ms;
    uint16_t epoch;
    char node_name[MESH_MAX_NODE_NAME + 1u];
};

/* PING/PONG：轻量探活时间戳。 */
struct mesh_ping_payload {
    uint32_t local_time_ms;
};

/* TIME_SYNC：四时间戳同步。 */
struct mesh_time_sync_payload {
    uint32_t t0_master_send;
    uint32_t t1_slave_recv;
    uint32_t t2_slave_send;
    uint32_t t3_master_recv;
};

/*
 * ROUTE_UPDATE：单条路由项更新。
 *
 * next_hop 字段在 v1 中更准确的语义是“发送选择器”：
 * - 对普通 mesh 转发表，它通常仍是下一跳节点地址；
 * - 对子机多串口 direct-table，它表示本地出口串口/端口编号。
 */
struct mesh_route_update_payload {
    uint8_t dst;
    uint8_t next_hop;
    uint8_t metric;
    uint16_t route_version;
    uint8_t action;
};

/*
 * LINK_STATE：邻居链路状态上报。
 *
 * local_port 表示：当前 src 节点若要发往 neighbor，应当从哪个本地串口发出。
 * 主机维护全图时，需要这个字段来为子机推导“dst -> 出口串口号”的路由表。
 */
struct mesh_link_state_payload {
    uint8_t neighbor;
    uint8_t link_up;
    uint8_t quality;
    uint8_t local_port;
};

/* ERROR：错误码 + 关联请求序号。 */
struct mesh_error_payload {
    uint16_t code;
    uint16_t related_seq;
};

/* 计算 CRC16-CCITT-FALSE（poly=0x1021, init=0xFFFF）。 */
uint16_t mesh_crc16_ccitt_false(const uint8_t *data, size_t len);

/* 判断 type 是否属于控制面消息。 */
bool mesh_is_control_type(uint8_t type);

/*
 * 通用封帧函数。
 * 返回 false 的典型原因：
 * - 参数为空。
 * - payload 超限。
 * - 输出缓冲区容量不足。
 */
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
    size_t *out_len);

/*
 * 解帧并校验完整性。
 * 校验项：Magic、长度一致性、CRC、版本号。
 */
bool mesh_decode_frame(const uint8_t *frame, size_t frame_len, struct mesh_frame_view *out_view);

/*
 * 转发辅助：根据输入帧计算下一跳应写入的 hop。
 * 当 hop 已耗尽（0）时返回 false，调用方应丢帧。
 */
bool mesh_prepare_forward(const struct mesh_frame_view *in_frame, uint8_t *out_hop);

/*
 * 构造携带 mini9P 的数据面帧。
 * 实现会强制清除 CONTROL 标志，避免与控制面混淆。
 */
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
    size_t *out_len);

/* 解析 mini9P 数据面 payload 视图。 */
bool mesh_parse_mini9p_payload(
    const struct mesh_frame_view *frame,
    const uint8_t **out_payload,
    uint16_t *out_len);

/* REGISTER 构造与解析。 */
bool mesh_build_register(
    uint8_t src,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_register_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);

bool mesh_parse_register(const struct mesh_frame_view *frame, struct mesh_register_payload *out_payload);

/* ASSIGN 构造与解析。 */
bool mesh_build_assign(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_assign_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);

bool mesh_parse_assign(const struct mesh_frame_view *frame, struct mesh_assign_payload *out_payload);

/* PING/PONG 构造与解析（type 仅允许 PING 或 PONG）。 */
bool mesh_build_ping(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    uint8_t type,
    const struct mesh_ping_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);

bool mesh_parse_ping(const struct mesh_frame_view *frame, struct mesh_ping_payload *out_payload);

/* TIME_SYNC 构造与解析。 */
bool mesh_build_time_sync(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_time_sync_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);

bool mesh_parse_time_sync(const struct mesh_frame_view *frame, struct mesh_time_sync_payload *out_payload);

/* ROUTE_UPDATE 构造与解析。 */
bool mesh_build_route_update(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_route_update_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);

bool mesh_parse_route_update(
    const struct mesh_frame_view *frame,
    struct mesh_route_update_payload *out_payload);

/* LINK_STATE 构造与解析。 */
bool mesh_build_link_state(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_link_state_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);

bool mesh_parse_link_state(const struct mesh_frame_view *frame, struct mesh_link_state_payload *out_payload);

/* ERROR 构造与解析。 */
bool mesh_build_error(
    uint8_t src,
    uint8_t dst,
    uint16_t seq,
    uint8_t hop,
    const struct mesh_error_payload *payload,
    uint8_t *out_frame,
    size_t out_cap,
    size_t *out_len);

bool mesh_parse_error(const struct mesh_frame_view *frame, struct mesh_error_payload *out_payload);

/* 将错误码转成稳定字符串，便于日志和诊断。 */
const char *mesh_error_name(uint16_t code);

#endif
