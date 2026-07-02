/*
 * pwos-shared/mesh2/pwos_mesh2_control.c
 *
 * mesh2 控制面消息体的编码/解码实现。
 *
 * 本文件负责把 C 结构体转换成固定长度的二进制 payload，以及把二进制 payload
 * 解析回 C 结构体。所有控制面消息（节点注册、地址分配、租约续期、链路状态、
 * 路由更新、主机通告）都通过这里的函数序列化和反序列化。
 *
 * 设计约束：
 * - 所有消息长度固定，便于资源受限的 MCU 解析，无需动态长度计算。
 * - 多字节字段统一使用小端序（little-endian）。
 * - payload[0] 固定为 PWOS_MESH2_CONTROL_VERSION，当前为 1，用于版本兼容。
 * - 保留字段填 0，未来扩展时可在保留位置添加新字段而不破坏布局。
 */

#include "pwos_mesh2_control.h"

#include <string.h>

/*
 * 辅助函数：把 16 位无符号整数以小端序写入 dst[0..1]。
 * dst 必须至少可写 2 字节。
 */
static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

/*
 * 辅助函数：从 src[0..1] 读取小端序 16 位无符号整数。
 */
static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

/*
 * 辅助函数：把 32 位无符号整数以小端序写入 dst[0..3]。
 * dst 必须至少可写 4 字节。
 */
static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

/*
 * 辅助函数：从 src[0..3] 读取小端序 32 位无符号整数。
 */
static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

/*
 * 编码前公共参数检查。
 *
 * 参数：
 *   msg          待编码的源结构体指针
 *   payload      输出缓冲区
 *   payload_cap  输出缓冲区容量
 *   required_len 该消息固定需要的 payload 长度
 *   out_len      输出实际写入字节数
 *
 * 返回：
 *   PWOS_OK           参数检查通过
 *   PWOS_E_NO_MEMORY  输入指针为空或缓冲区容量不足
 */
static pwos_status_t check_encode_args(
    const void *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t required_len,
    size_t *out_len)
{
    if (msg == NULL || payload == NULL || out_len == NULL) {
        return PWOS_E_NO_MEMORY;
    }
    if (payload_cap < required_len) {
        return PWOS_E_NO_MEMORY;
    }
    return PWOS_OK;
}

/*
 * 解码前公共参数检查。
 *
 * 检查项目：
 * - 输入指针非空
 * - payload_len 必须严格等于该消息固定长度（固定长度协议）
 * - payload[0] 必须是 PWOS_MESH2_CONTROL_VERSION
 *
 * 返回：
 *   PWOS_OK          参数检查通过
 *   PWOS_E_NO_MEMORY 输入指针为空
 *   PWOS_E_BAD_LENGTH 长度不匹配或版本号错误
 */
static pwos_status_t check_decode_args(
    const uint8_t *payload,
    size_t payload_len,
    size_t required_len,
    void *out_msg)
{
    if (payload == NULL || out_msg == NULL) {
        return PWOS_E_NO_MEMORY;
    }
    if (payload_len != required_len || payload[0] != PWOS_MESH2_CONTROL_VERSION) {
        return PWOS_E_BAD_LENGTH;
    }
    return PWOS_OK;
}

/*
 * 编码 NODE_REGISTER 消息。
 *
 * 消息方向：Slave -> Host（协调器）
 * 作用：未分配节点或重启后的节点向上游/协调器注册身份，请求加入网络。
 *
 * payload 布局（共 24 字节）：
 *   offset  size  字段
 *      0     1    version = 1
 *      1     1    upstream_port
 *      2     2    保留（填 0）
 *      4     4    caps（小端）
 *      8     4    boot_id（小端）
 *     12     4    uid[0]（小端）
 *     16     4    uid[1]（小端）
 *     20     4    uid[2]（小端）
 */
pwos_status_t pwos_mesh2_encode_node_register(
    const pwos_mesh2_node_register_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }

    memset(payload, 0, PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->upstream_port;
    put_le32(payload + 4u, msg->caps);
    put_le32(payload + 8u, msg->boot_id);
    put_le32(payload + 12u, msg->uid[0]);
    put_le32(payload + 16u, msg->uid[1]);
    put_le32(payload + 20u, msg->uid[2]);
    *out_len = PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN;
    return PWOS_OK;
}

/*
 * 解码 NODE_REGISTER 消息。
 *
 * 参数：
 *   payload     收到的 payload 数据
 *   payload_len payload 长度，必须等于 24
 *   out_msg     输出解析后的结构体
 */
pwos_status_t pwos_mesh2_decode_node_register(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_node_register_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->upstream_port = payload[1];
    out_msg->caps = get_le32(payload + 4u);
    out_msg->boot_id = get_le32(payload + 8u);
    out_msg->uid[0] = get_le32(payload + 12u);
    out_msg->uid[1] = get_le32(payload + 16u);
    out_msg->uid[2] = get_le32(payload + 20u);
    return PWOS_OK;
}

/*
 * 编码 ADDR_ASSIGN 消息。
 *
 * 消息方向：Host（协调器） -> Slave
 * 作用：协调器给节点分配短地址、租约时长和租约 epoch。
 *
 * payload 布局（共 28 字节）：
 *   offset  size  字段
 *      0     1    version = 1
 *      1     1    addr（分配的短地址）
 *      2     1    flags（如 PWOS_MESH2_ASSIGN_FLAG_OK）
 *      3     1    保留（填 0）
 *      4     4    lease_epoch（小端）
 *      8     4    lease_ms（小端）
 *     12     4    boot_id（小端）
 *     16     4    uid[0]（小端）
 *     20     4    uid[1]（小端）
 *     24     4    uid[2]（小端）
 */
pwos_status_t pwos_mesh2_encode_addr_assign(
    const pwos_mesh2_addr_assign_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }

    memset(payload, 0, PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->addr;
    payload[2] = msg->flags;
    put_le32(payload + 4u, msg->lease_epoch);
    put_le32(payload + 8u, msg->lease_ms);
    put_le32(payload + 12u, msg->boot_id);
    put_le32(payload + 16u, msg->uid[0]);
    put_le32(payload + 20u, msg->uid[1]);
    put_le32(payload + 24u, msg->uid[2]);
    *out_len = PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN;
    return PWOS_OK;
}

/*
 * 解码 ADDR_ASSIGN 消息。
 */
pwos_status_t pwos_mesh2_decode_addr_assign(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_addr_assign_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->addr = payload[1];
    out_msg->flags = payload[2];
    out_msg->lease_epoch = get_le32(payload + 4u);
    out_msg->lease_ms = get_le32(payload + 8u);
    out_msg->boot_id = get_le32(payload + 12u);
    out_msg->uid[0] = get_le32(payload + 16u);
    out_msg->uid[1] = get_le32(payload + 20u);
    out_msg->uid[2] = get_le32(payload + 24u);
    return PWOS_OK;
}

/*
 * 编码 LEASE_RENEW 消息。
 *
 * 消息方向：Slave -> Host（协调器）
 * 作用：节点在租约到期前请求续租，证明自己仍然在线。
 *
 * payload 布局（共 24 字节）：
 *   offset  size  字段
 *      0     1    version = 1
 *      1     1    addr
 *      2     2    保留（填 0）
 *      4     4    lease_epoch（小端）
 *      8     4    boot_id（小端）
 *     12     4    uid[0]（小端）
 *     16     4    uid[1]（小端）
 *     20     4    uid[2]（小端）
 */
pwos_status_t pwos_mesh2_encode_lease_renew(
    const pwos_mesh2_lease_renew_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }

    memset(payload, 0, PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->addr;
    put_le32(payload + 4u, msg->lease_epoch);
    put_le32(payload + 8u, msg->boot_id);
    put_le32(payload + 12u, msg->uid[0]);
    put_le32(payload + 16u, msg->uid[1]);
    put_le32(payload + 20u, msg->uid[2]);
    *out_len = PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN;
    return PWOS_OK;
}

/*
 * 解码 LEASE_RENEW 消息。
 */
pwos_status_t pwos_mesh2_decode_lease_renew(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_lease_renew_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->addr = payload[1];
    out_msg->lease_epoch = get_le32(payload + 4u);
    out_msg->boot_id = get_le32(payload + 8u);
    out_msg->uid[0] = get_le32(payload + 12u);
    out_msg->uid[1] = get_le32(payload + 16u);
    out_msg->uid[2] = get_le32(payload + 20u);
    return PWOS_OK;
}

/*
 * 编码 LEASE_ACK 消息。
 *
 * 消息方向：Host（协调器） -> Slave
 * 作用：续租确认，格式与 ADDR_ASSIGN 完全相同。
 * 实现上直接复用 ADDR_ASSIGN 的编码函数。
 */
pwos_status_t pwos_mesh2_encode_lease_ack(
    const pwos_mesh2_lease_ack_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    return pwos_mesh2_encode_addr_assign(msg, payload, payload_cap, out_len);
}

/*
 * 解码 LEASE_ACK 消息。
 * 格式与 ADDR_ASSIGN 相同，直接复用其解码函数。
 */
pwos_status_t pwos_mesh2_decode_lease_ack(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_lease_ack_t *out_msg)
{
    return pwos_mesh2_decode_addr_assign(payload, payload_len, out_msg);
}

/*
 * 编码 LINK_STATE 消息。
 *
 * 消息方向：Slave -> Host（协调器）
 * 作用：节点向协调器上报自己每个端口的直连邻居状态，
 *      协调器据此计算全网拓扑并下发路由。
 *
 * payload 布局（共 24 字节）：
 *   offset  size  字段
 *      0     1    version = 1
 *      1     1    local_addr（本节点短地址）
 *      2     1    local_port（本端口号）
 *      3     1    peer_addr（对端短地址）
 *      4     1    peer_port（对端端口号）
 *      5     1    flags（如 UP/DOWN）
 *      6     2    metric（小端，当前固定为 1）
 *      8     4    peer_boot_id（小端）
 *     12     4    peer_uid[0]（小端）
 *     16     4    peer_uid[1]（小端）
 *     20     4    peer_uid[2]（小端）
 */
pwos_status_t pwos_mesh2_encode_link_state(
    const pwos_mesh2_link_state_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_LINK_STATE_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }

    memset(payload, 0, PWOS_MESH2_LINK_STATE_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->local_addr;
    payload[2] = msg->local_port;
    payload[3] = msg->peer_addr;
    payload[4] = msg->peer_port;
    payload[5] = msg->flags;
    put_le16(payload + 6u, msg->metric);
    put_le32(payload + 8u, msg->peer_boot_id);
    put_le32(payload + 12u, msg->peer_uid[0]);
    put_le32(payload + 16u, msg->peer_uid[1]);
    put_le32(payload + 20u, msg->peer_uid[2]);
    *out_len = PWOS_MESH2_LINK_STATE_PAYLOAD_LEN;
    return PWOS_OK;
}

/*
 * 解码 LINK_STATE 消息。
 */
pwos_status_t pwos_mesh2_decode_link_state(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_link_state_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_LINK_STATE_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->local_addr = payload[1];
    out_msg->local_port = payload[2];
    out_msg->peer_addr = payload[3];
    out_msg->peer_port = payload[4];
    out_msg->flags = payload[5];
    out_msg->metric = get_le16(payload + 6u);
    out_msg->peer_boot_id = get_le32(payload + 8u);
    out_msg->peer_uid[0] = get_le32(payload + 12u);
    out_msg->peer_uid[1] = get_le32(payload + 16u);
    out_msg->peer_uid[2] = get_le32(payload + 20u);
    return PWOS_OK;
}

/*
 * 编码 ROUTE_UPDATE 消息。
 *
 * 消息方向：Host（协调器） -> Slave
 * 作用：协调器向节点下发一条路由表项，告知到某个目标地址应该怎么走。
 *
 * payload 布局（共 12 字节）：
 *   offset  size  字段
 *      0     1    version = 1
 *      1     1    dst（目标短地址）
 *      2     1    next_hop（下一跳短地址）
 *      3     1    action（SET=1 或 DELETE=2）
 *      4     4    route_version（小端，用于防乱序覆盖）
 *      8     2    metric（小端，跳数）
 *     10     2    保留（填 0）
 */
pwos_status_t pwos_mesh2_encode_route_update(
    const pwos_mesh2_route_update_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status;

    status = check_encode_args(
        msg, payload, payload_cap, PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN, out_len);
    if (status != PWOS_OK) {
        return status;
    }
    if (msg->action != PWOS_MESH2_ROUTE_SET && msg->action != PWOS_MESH2_ROUTE_DELETE) {
        return PWOS_E_BAD_LENGTH;
    }

    memset(payload, 0, PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->dst;
    payload[2] = msg->next_hop;
    payload[3] = msg->action;
    put_le32(payload + 4u, msg->route_version);
    put_le16(payload + 8u, msg->metric);
    *out_len = PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN;
    return PWOS_OK;
}

/*
 * 解码 ROUTE_UPDATE 消息。
 * 同时校验 action 字段是否合法。
 */
pwos_status_t pwos_mesh2_decode_route_update(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_route_update_t *out_msg)
{
    pwos_status_t status;

    status = check_decode_args(
        payload, payload_len, PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN, out_msg);
    if (status != PWOS_OK) {
        return status;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->dst = payload[1];
    out_msg->next_hop = payload[2];
    out_msg->action = payload[3];
    out_msg->route_version = get_le32(payload + 4u);
    out_msg->metric = get_le16(payload + 8u);
    if (out_msg->action != PWOS_MESH2_ROUTE_SET &&
        out_msg->action != PWOS_MESH2_ROUTE_DELETE) {
        return PWOS_E_BAD_LENGTH;
    }
    return PWOS_OK;
}

/*
 * 编码 HOST_ADVERTISE 消息。
 *
 * 消息方向：Host -> Slave（广播/泛洪）
 * 作用：多主机场景下，ESP32 主机向 STM32 平面通告自己的角色
 *      （observer/follower/leader）、epoch、priority、host_uid 和 cluster_id。
 *      STM32 节点据此选择应该服从哪个主机的权威。
 *
 * payload 布局（共 28 字节）：
 *   offset  size  字段
 *      0     1    version = 1
 *      1     1    role（0=observer, 1=follower, 2=leader）
 *      2     1    flags
 *      3     1    保留（填 0）
 *      4     4    epoch（小端）
 *      8     2    priority（小端）
 *     10     2    保留（填 0）
 *     12     4    host_uid[0]（小端）
 *     16     4    host_uid[1]（小端）
 *     20     4    host_uid[2]（小端）
 *     24     4    cluster_id（小端）
 */
pwos_status_t pwos_mesh2_encode_host_advertise(
    const pwos_mesh2_host_advertise_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len)
{
    pwos_status_t status = check_encode_args(
        msg, payload, payload_cap,
        PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN, out_len);

    if (status != PWOS_OK) {
        return status;
    }
    if (msg->role > PWOS_MESH2_HOST_ROLE_LEADER) {
        return PWOS_E_BAD_LENGTH;
    }
    memset(payload, 0, PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN);
    payload[0] = PWOS_MESH2_CONTROL_VERSION;
    payload[1] = msg->role;
    payload[2] = msg->flags;
    put_le32(payload + 4u, msg->epoch);
    put_le16(payload + 8u, msg->priority);
    put_le32(payload + 12u, msg->host_uid[0]);
    put_le32(payload + 16u, msg->host_uid[1]);
    put_le32(payload + 20u, msg->host_uid[2]);
    put_le32(payload + 24u, msg->cluster_id);
    *out_len = PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN;
    return PWOS_OK;
}

/*
 * 解码 HOST_ADVERTISE 消息。
 * 同时校验 role 字段是否合法。
 */
pwos_status_t pwos_mesh2_decode_host_advertise(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_host_advertise_t *out_msg)
{
    pwos_status_t status = check_decode_args(
        payload, payload_len,
        PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN, out_msg);

    if (status != PWOS_OK) {
        return status;
    }
    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->role = payload[1];
    out_msg->flags = payload[2];
    out_msg->epoch = get_le32(payload + 4u);
    out_msg->priority = get_le16(payload + 8u);
    out_msg->host_uid[0] = get_le32(payload + 12u);
    out_msg->host_uid[1] = get_le32(payload + 16u);
    out_msg->host_uid[2] = get_le32(payload + 20u);
    out_msg->cluster_id = get_le32(payload + 24u);
    if (out_msg->role > PWOS_MESH2_HOST_ROLE_LEADER) {
        return PWOS_E_BAD_LENGTH;
    }
    return PWOS_OK;
}
