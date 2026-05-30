/**
 * @file mesh_node_runtime.h
 * @brief STM32 slave 侧 mesh 节点运行时。
 *
 * mesh_node_runtime 连接 shared mesh_processer、direct-table cluster 和本地
 * Mini9P server。它负责节点 bootstrap REGISTER、ASSIGN 落地、本机地址同步、
 * ingress-port 感知处理，以及把“某个 mesh 地址从哪个端口进入”上报给 service。
 *
 * runtime 不直接拥有 UART transport；底层收发通过 send_frame/receive_frame
 * 回调注入。多 UART 端口和 addr-port 映射由 mesh_node_service 管理。
 */

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

/**
 * @brief mesh_node_runtime 初始化配置。
 */
struct mesh_node_runtime_config {
    /** 原始 mesh 帧发送函数，不可为 NULL。next_hop 使用 cluster 返回的 mesh 地址语义。 */
    mesh_processer_send_frame_fn send_frame;
    /** 原始 mesh 帧接收函数，不可为 NULL，必须返回入口端口或 MESH_PROCESSER_INGRESS_PORT_NONE。 */
    mesh_processer_receive_frame_fn receive_frame;
    /** 原样传给 send_frame/receive_frame 的底层 transport 上下文。 */
    void *transport_ctx;
    /** 可选回调：当 runtime 从 ingress_port 学到 mesh_addr 可达时通知上层 service。 */
    int (*learn_peer_port)(void *ctx, uint8_t mesh_addr, uint8_t port_id);
    /** 原样传给 learn_peer_port 的上下文。 */
    void *learn_peer_port_ctx;
    /** 本地 Mini9P server 帧处理器；不需要数据面服务时可为 NULL。 */
    mesh_processer_mini9p_server_handler_fn mini9p_server_handler;
    /** 原样传给 mini9p_server_handler 的上下文。 */
    void *mini9p_server_ctx;
    /** REGISTER 中通告的稳定硬件 UID。 */
    uint8_t local_uid[MESH_UID_LEN];
    /** REGISTER 中通告的本次启动随机/时序 nonce。 */
    uint32_t boot_nonce;
    /** REGISTER 中通告的能力位。 */
    uint16_t capability_bits;
    /** REGISTER 中通告的本机可用端口位图。 */
    uint8_t port_bitmap;
    /** bootstrap REGISTER 使用的 next_hop；通常为 broadcast selector。 */
    uint8_t bootstrap_next_hop;
    /** 初始本机 mesh 地址；未分配地址时通常为 MESH_ADDR_UNASSIGNED。 */
    uint8_t local_addr;
    /** 本机发起 REGISTER 或响应帧时使用的默认 hop。 */
    uint8_t default_hop;
    /** init 成功后是否立即发送一次 REGISTER。 */
    bool auto_register_on_init;
};

/**
 * @brief mesh_node_runtime 实例状态。
 */
struct mesh_node_runtime {
    /** 当前生效配置。 */
    struct mesh_node_runtime_config config;
    /** direct-table cluster，保存本机地址和 dst->next_hop(mesh addr) 路由。 */
    struct cluster cluster;
    /** shared mesh processor，负责帧解码、本机命中、转发和 Mini9P 分发。 */
    struct mesh_processer processor;
    /** 是否已完成初始化。 */
    bool initialized;
    /** 下一帧由 runtime 主动生成的 mesh seq。 */
    uint16_t next_mesh_seq;
};

/**
 * @brief 填充 runtime 默认配置。
 *
 * 默认 local_addr 为 MESH_ADDR_UNASSIGNED，default_hop 为
 * MESH_PROCESSER_DEFAULT_HOP，并启用 auto_register_on_init。
 *
 * @param[out] out_config 要填充的配置；为 NULL 时无操作。
 */
void mesh_node_runtime_get_default_config(struct mesh_node_runtime_config *out_config);

/**
 * @brief 初始化 mesh_node_runtime。
 *
 * 初始化会创建 direct-table cluster 和 shared mesh processor。若
 * auto_register_on_init 为 true，会立即按 bootstrap_next_hop 发送 REGISTER。
 *
 * @param[out] runtime runtime 实例。
 * @param[in] config 调用方配置；send_frame 和 receive_frame 不可为 NULL。
 * @return 0 表示成功；负的 MESH_ERR_* 表示失败。
 */
int mesh_node_runtime_init(
    struct mesh_node_runtime *runtime,
    const struct mesh_node_runtime_config *config);

/**
 * @brief 反初始化 runtime，并清空内部状态。
 *
 * @param[in,out] runtime runtime 实例；为 NULL 时无操作。
 */
void mesh_node_runtime_deinit(struct mesh_node_runtime *runtime);

/**
 * @brief 通知链路恢复，并主动发送一次 REGISTER。
 *
 * @param[in,out] runtime runtime 实例。
 * @return 0 表示 REGISTER 已提交到底层发送回调；负的 MESH_ERR_* 表示失败。
 */
int mesh_node_runtime_notify_link_up(struct mesh_node_runtime *runtime);

/**
 * @brief 处理一帧 mesh 数据，入口端口未知。
 *
 * 等价于 mesh_node_runtime_process_frame_from_port(...,
 * MESH_PROCESSER_INGRESS_PORT_NONE)。
 *
 * @param[in,out] runtime runtime 实例。
 * @param[in] frame_data 完整 mesh 帧。
 * @param[in] frame_len mesh 帧长度。
 * @return 0 表示处理成功；负的 MESH_ERR_* 表示失败。
 */
int mesh_node_runtime_process_frame(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len);

/**
 * @brief 处理一帧 mesh 数据，并携带该帧进入的 UART 端口。
 *
 * runtime 会先刷新 direct-table 路由 `src -> src`，并通过 learn_peer_port
 * 回调把 `src -> ingress_port` 通知给 service；随后交给 mesh_processer
 * 做本机分发或转发。
 *
 * @param[in,out] runtime runtime 实例。
 * @param[in] frame_data 完整 mesh 帧。
 * @param[in] frame_len mesh 帧长度。
 * @param[in] ingress_port 收到该帧的端口索引，未知时传 MESH_PROCESSER_INGRESS_PORT_NONE。
 * @return 0 表示处理成功；负的 MESH_ERR_* 表示失败。
 */
int mesh_node_runtime_process_frame_from_port(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len,
    uint8_t ingress_port);

/**
 * @brief 通过 receive_frame 读取并处理最多一帧 mesh 数据。
 *
 * @param[in,out] runtime runtime 实例。
 * @return 0 表示处理了一帧；负的 MESH_ERR_* 表示暂无帧或处理失败。
 */
int mesh_node_runtime_poll_once(struct mesh_node_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
