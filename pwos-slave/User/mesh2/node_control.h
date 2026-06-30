#ifndef PWOS_NODE_CONTROL_H
#define PWOS_NODE_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#include "frame_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_NODE_CONTROL_MAX_ROUTES 16u
#define PWOS_NODE_CONTROL_MAX_PENDING 8u

typedef enum {
    PWOS_NODE_OFFLINE = 0,
    PWOS_NODE_REGISTERING,
    PWOS_NODE_ASSIGNED,
    PWOS_NODE_LIMITED
} pwos_node_state_t;

typedef struct {
    uint8_t valid;
    uint8_t dst;
    uint8_t next_hop;
    uint8_t port_id;
    uint16_t metric;
    uint32_t route_version;
} pwos_node_route_snapshot_t;

typedef struct {
    pwos_node_state_t state;
    uint8_t local_addr;
    uint8_t upstream_port;
    uint32_t local_uid[3];
    uint32_t local_boot_id;
    uint32_t lease_epoch;
    uint32_t lease_ms;
    uint32_t last_register_tick;
    uint32_t last_renew_tick;
    uint32_t register_tx;
    uint32_t assign_rx;
    uint32_t renew_tx;
    uint32_t lease_ack_rx;
    uint32_t link_state_tx;
    uint32_t route_update_rx;
    uint32_t forwarded_ctrl;
    uint32_t relay_register_rx;
    uint32_t relay_register_forward_tx;
    uint32_t relay_register_drop_not_assigned;
    uint32_t relay_assign_forward_tx;
    uint32_t forward_fail;
    uint32_t drop_no_route;
    uint32_t bad_ctrl_frames;
    uint32_t local_data_tx;
    uint32_t local_data_tx_fail;
} pwos_node_control_snapshot_t;

void pwos_node_control_init(void);
void pwos_node_control_tick(void);

/*
 * 处理 mesh 控制面/数据面帧。
 *
 * 返回 1 表示已经消费 block，调用方必须释放；返回 0 表示不是本模块处理范围。
 */
int pwos_node_control_handle_rx(pwos_frame_block_t *block);

int pwos_node_control_route_lookup(uint8_t dst, uint8_t *out_port_id);

/*
 * 从本机 service 发送一帧数据面 payload。
 *
 * node_control 拥有本机 mesh 地址和路由表，因此 service 只提交目的地址和
 * payload，不直接选择 UART 端口。
 */
int pwos_node_control_send_data(
    uint8_t type,
    uint8_t dst,
    const uint8_t *payload,
    uint16_t payload_len);

void pwos_node_control_get_snapshot(pwos_node_control_snapshot_t *out);
size_t pwos_node_control_get_routes(
    pwos_node_route_snapshot_t *out,
    size_t out_capacity);

const char *pwos_node_state_name(pwos_node_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_NODE_CONTROL_H */
