#ifndef MESH_NODE_RUNTIME_H
#define MESH_NODE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../cluster/cluster.h"
#include "../processer/mesh_processer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mesh node runtime：节点侧最小运行时装配层。
 *
 * 这一层解决的是“shared mesh processor 已经能分流，但节点到底何时发首个
 * REGISTER、何时把 ASSIGN 落成正式地址、以及本地 mini9P server 怎样接进
 * mesh 数据面”的问题。
 *
 * 设计边界：
 * 1. 每个 runtime 实例只代表一条物理链路（例如一条 UART）。
 * 2. runtime 自己维护一份最小 direct-table cluster，用于：
 *    - 保存本机地址；
 *    - 记住“直接对端 src 可从当前链路到达”；
 *    - 给 processor 的本机/下一跳判断提供回答。
 * 3. 节点收到 ASSIGN 后，会同时更新 cluster.local_addr 与 processor.local_addr，
 *    保证后续本机回包不再继续使用 0xFF。
 */

struct mesh_node_runtime_config {
    mesh_processer_send_frame_fn send_frame; /**< 原始链路发帧函数，不可为 NULL。 */
    mesh_processer_receive_frame_fn receive_frame; /**< 原始链路收帧函数，不可为 NULL。 */
    void *transport_ctx; /**< 原样传给 send/receive 的链路上下文。 */
    mesh_processer_mini9p_server_handler_fn mini9p_server_handler; /**< 本地 mini9P server 处理器。 */
    void *mini9p_server_ctx; /**< 本地 mini9P server 上下文。 */
    uint8_t local_uid[MESH_UID_LEN]; /**< 当前链路上对外通告的稳定硬件 UID。 */
    uint32_t boot_nonce; /**< 本次上电的 boot nonce。 */
    uint16_t capability_bits; /**< REGISTER 里的能力位图。 */
    uint8_t port_bitmap; /**< REGISTER 里的端口位图。 */
    uint8_t bootstrap_next_hop; /**< 首次 REGISTER 要发往的首跳；点对点 UART 可忽略。 */
    uint8_t local_addr; /**< 初始本机地址；通常为 MESH_ADDR_UNASSIGNED。 */
    uint8_t default_hop; /**< 本机发起 REGISTER/回包时的默认 hop。 */
    bool auto_register_on_init; /**< init 成功后是否立即按当前链路发送一次 REGISTER。 */
};

struct mesh_node_runtime {
    struct mesh_node_runtime_config config; /**< 当前生效配置。 */
    struct cluster cluster; /**< 每条链路的最小 direct-table cluster。 */
    struct mesh_processer processor; /**< 本链路对应的 shared mesh processor。 */
    bool initialized; /**< 是否已完成初始化。 */
    uint16_t next_mesh_seq; /**< 本链路下次要使用的 mesh 序号。 */
};

void mesh_node_runtime_get_default_config(struct mesh_node_runtime_config *out_config);

int mesh_node_runtime_init(
    struct mesh_node_runtime *runtime,
    const struct mesh_node_runtime_config *config);

void mesh_node_runtime_deinit(struct mesh_node_runtime *runtime);

/*
 * 把“当前串口链路已经连通”显式通知给 runtime。
 *
 * 该接口会立刻向本链路发送一帧 REGISTER，并携带当前配置中的硬件 UID。
 * 如果调用方把它放在 UART ready / cable connected / 邻居上线等事件上，
 * 就能实现“以串口为单位，链路一连通就发 REGISTER”。
 */
int mesh_node_runtime_notify_link_up(struct mesh_node_runtime *runtime);

int mesh_node_runtime_process_frame(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len);

int mesh_node_runtime_process_frame_from_port(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len,
    uint8_t ingress_port);

int mesh_node_runtime_poll_once(struct mesh_node_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
