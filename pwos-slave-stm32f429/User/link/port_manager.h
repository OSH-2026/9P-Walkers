#ifndef PWOS_PORT_MANAGER_H
#define PWOS_PORT_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "frame_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PWOS_PORT_DISABLED = 0,
    PWOS_PORT_PROBING,
    PWOS_PORT_LINK_UP,
    PWOS_PORT_HOST_CANDIDATE,
    PWOS_PORT_UPSTREAM,
    PWOS_PORT_PEER,
    PWOS_PORT_SUSPECT,
    PWOS_PORT_QUARANTINED
} pwos_port_state_t;

typedef enum {
    PWOS_PORT_PEER_UNKNOWN = 0,
    PWOS_PORT_PEER_NODE = 1,
    PWOS_PORT_PEER_COORDINATOR = 2
} pwos_port_peer_role_t;

#define PWOS_PORT_CAP_COORDINATOR 0x01u
#define PWOS_PORT_CAP_RELAY 0x02u
#define PWOS_PORT_CAP_UPSTREAM_REACHABLE 0x04u

typedef struct {
    uint8_t id;
    const char *name;
    pwos_port_state_t state;
    pwos_port_peer_role_t peer_role;
    uint8_t peer_port_id;
    uint8_t peer_addr;
    uint8_t peer_caps;
    uint32_t peer_boot_id;
    uint32_t peer_uid[3];
    uint32_t last_rx_tick;
    uint32_t last_tx_tick;
    uint32_t hello_tx;
    uint32_t hello_rx;
    uint32_t hello_ack_tx;
    uint32_t hello_ack_rx;
    uint32_t bad_link_frames;
} pwos_port_snapshot_t;

/*
 * 初始化端口管理器。
 *
 * 调用上下文：scheduler 启动前，在 pwos_uart_dma_ports_init() 前后均可。
 */
void pwos_port_manager_init(void);

/*
 * 周期推进端口 FSM，并对每个可用端口发送 LINK_HELLO。
 *
 * 调用上下文：port_mgr_task。
 */
void pwos_port_manager_tick(void);

/*
 * 尝试处理一帧链路维护帧。
 *
 * 调用上下文：link_rx_task。
 * 返回：1 表示已消费，调用方应释放 block；0 表示不是端口管理器关心的帧。
 */
int pwos_port_manager_handle_rx(pwos_frame_block_t *block);

/*
 * 从当前端口表里选择控制面上游候选。
 *
 * 优先选择直连 coordinator，其次选择已经声明可达上游的 relay 节点。
 * 返回 0 表示找到端口；负数表示当前还没有可用上游。
 */
int pwos_port_manager_select_upstream(uint8_t *out_port_id);

/*
 * 标记某个端口为当前控制面 upstream。
 *
 * 调用上下文：mesh_ctrl_task。用于收到 ADDR_ASSIGN 后固定本节点上游端口。
 */
void pwos_port_manager_mark_upstream(uint8_t port_id);

/*
 * 通知 port_manager 本节点已经获得 mesh 地址。
 *
 * 获得地址后 HELLO 会携带 UPSTREAM_REACHABLE capability，使下游节点可以选择
 * 本节点作为注册中继。
 */
void pwos_port_manager_set_mesh_assigned(uint8_t assigned, uint8_t local_addr);

/*
 * 获取端口状态快照。
 *
 * 调用上下文：任务；返回写入 out 的条目数。
 */
void pwos_port_manager_set_peer_addr(uint8_t port_id, uint8_t peer_addr);
int pwos_port_manager_find_port_by_peer_addr(uint8_t peer_addr);

size_t pwos_port_manager_get_snapshot(
    pwos_port_snapshot_t *out,
    size_t out_capacity);

const char *pwos_port_state_name(pwos_port_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_PORT_MANAGER_H */
