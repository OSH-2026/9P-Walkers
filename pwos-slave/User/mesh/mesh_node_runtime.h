#ifndef MESH_NODE_RUNTIME_H
#define MESH_NODE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../pwos-shared/mesh/cluster/cluster.h"
#include "../../../pwos-shared/mesh/processer/mesh_processer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mesh node runtime：节点侧的多端口运行时装配层。
 *
 * 这一层负责把：
 * - 一个 shared mesh_processer；
 * - 一个 direct-table cluster；
 * - 多个物理串口/链路；
 * - 一个本地 mini9P server；
 *
 * 串成“单 runtime 管理本节点所有串口”的可轮询闭环。
 *
 * 设计目标：
 * 1. runtime 自己统一轮询多个 UART，而不是为每条 UART 各起一个 runtime；
 * 2. 子机 cluster 的 direct-table 路由表保存“dst -> 本地出口端口号”；
 * 3. processor 仍维持原有 send_frame/receive_frame 接口，不要求 shared mesh
 *    层感知多端口细节；
 * 4. 当节点收到 ASSIGN 后，会同时更新 cluster.local_addr 与 processor.local_addr，
 *    确保后续回包不再继续使用 0xFF。
 */

/* 当前单个 runtime 最多管理的本地物理端口数。 */
#define MESH_NODE_RUNTIME_MAX_PORTS 8u

/* 无效或未知的本地端口编号。 */
#define MESH_NODE_RUNTIME_INVALID_PORT CLUSTER_PORT_INVALID

/*
 * 一个物理端口的静态配置。
 *
 * send_frame/receive_frame 与 shared mesh_processer 使用的签名保持一致，
 * 这样 runtime 内部既可以包装真实 UART，也可以在主机测试里直接挂 fake transport。
 */
struct mesh_node_runtime_port_config {
    mesh_processer_send_frame_fn send_frame;     /**< 绑定到该物理端口的原始发帧函数。 */
    mesh_processer_receive_frame_fn receive_frame; /**< 绑定到该物理端口的原始收帧函数。 */
    void *transport_ctx;                         /**< 原样传给该端口 send/receive 的上下文。 */
    uint8_t port_id;                            /**< runtime 内部使用的稳定本地端口编号。 */
};

/* 运行时内部保存的一条端口绑定。 */
struct mesh_node_runtime_port {
    bool initialized;                           /**< 该端口是否已成功注册到 runtime。 */
    uint8_t port_id;                            /**< 本地端口编号。 */
    mesh_processer_send_frame_fn send_frame;    /**< 该端口的底层发帧函数。 */
    mesh_processer_receive_frame_fn receive_frame; /**< 该端口的底层收帧函数。 */
    void *transport_ctx;                        /**< 该端口的链路上下文。 */
};

struct mesh_node_runtime_config {
    /*
     * 多端口主路径：
     * - ports 指向长度至少为 port_count 的端口配置数组；
     * - runtime_init() 会按端口数组逐个绑定、编号并纳入轮询。
     */
    const struct mesh_node_runtime_port_config *ports;

    /*
     * 兼容旧版的单端口快捷字段。
     * 当 ports == NULL 且 runtime_init(..., port_count=1) 时，runtime 会退化为
     * 仅使用这一组 send/receive/transport_ctx，端口编号固定为 0。
     */
    mesh_processer_send_frame_fn send_frame;
    mesh_processer_receive_frame_fn receive_frame;
    void *transport_ctx;

    mesh_processer_mini9p_server_handler_fn mini9p_server_handler; /**< 本地 mini9P server 处理器。 */
    void *mini9p_server_ctx;                                      /**< 传给 mini9P server 的上下文。 */
    uint8_t local_uid[MESH_UID_LEN];                              /**< 当前节点对外通告的稳定硬件 UID。 */
    uint32_t boot_nonce;                                          /**< 本次上电对应的 boot nonce。 */
    uint16_t capability_bits;                                     /**< REGISTER 中的能力位图。 */
    uint8_t port_bitmap;                                          /**< REGISTER 中的端口位图；为 0 时按 ports 自动生成。 */
    uint8_t local_addr;                                           /**< 初始本机地址；通常为 MESH_ADDR_UNASSIGNED。 */
    uint8_t default_hop;                                          /**< 本机发起 REGISTER/回包时的默认 hop。 */
    bool auto_register_on_init;                                   /**< init 成功后是否自动向所有端口发送一次 REGISTER。 */
};

struct mesh_node_runtime {
    struct mesh_node_runtime_config config;             /**< 当前生效的初始化配置快照。 */
    struct cluster cluster;                            /**< 子机侧 direct-table cluster。 */
    struct mesh_processer processor;                   /**< 统一处理所有端口流量的 shared processor。 */
    bool initialized;                                  /**< 是否完成初始化。 */
    uint16_t next_mesh_seq;                            /**< 本节点下次发出的 mesh 序号。 */
    size_t port_count;                                 /**< 当前 runtime 管理的本地端口数量。 */
    size_t next_rx_port_index;                         /**< 下一轮轮询从哪个端口开始。 */
    uint8_t active_rx_port;                            /**< 当前正在被 processor 处理的入端口编号。 */
    uint8_t control_plane_port;                        /**< 当前已知的上行控制面出口端口。 */
    struct mesh_node_runtime_port ports[MESH_NODE_RUNTIME_MAX_PORTS]; /**< 已绑定端口表。 */
};

/* 填充安全默认值：未分配地址、默认 hop、自动 init 后广播 REGISTER。 */
void mesh_node_runtime_get_default_config(struct mesh_node_runtime_config *out_config);

/*
 * 初始化一个多端口 node runtime。
 *
 * @param runtime 待初始化实例。
 * @param config  初始化配置。
 * @param port_count 当前 MCU 节点需要由该 runtime 管理的物理串口/端口数量。
 *
 * 行为：
 * - 若 config->ports != NULL，则按端口数组逐个绑定端口；
 * - 若 config->ports == NULL 且 port_count == 1，则退化为旧版单端口模式；
 * - runtime 会把 cluster 配成 DIRECT_TABLE + 端口选择器语义；
 * - auto_register_on_init=true 时，会向所有已绑定端口各发送一次 REGISTER。
 */
int mesh_node_runtime_init(
    struct mesh_node_runtime *runtime,
    const struct mesh_node_runtime_config *config,
    size_t port_count);

/* 清理 runtime、processor、cluster 和端口绑定状态。 */
void mesh_node_runtime_deinit(struct mesh_node_runtime *runtime);

/*
 * 显式通知“所有已绑定链路已连通”，向每个端口各发送一次 REGISTER。
 */
int mesh_node_runtime_notify_link_up(struct mesh_node_runtime *runtime);

/*
 * 显式通知“某个端口链路已连通”，只在该端口上发送一次 REGISTER。
 */
int mesh_node_runtime_notify_link_up_on_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id);

/*
 * 处理一帧已经读入内存的 mesh 数据，但不提供入端口信息。
 *
 * 该兼容接口保留给旧调用方；若需要让 runtime 自动把 frame.src 记成
 * “可通过哪个本地串口到达”，请优先使用 process_frame_on_port()。
 */
int mesh_node_runtime_process_frame(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len);

/*
 * 处理一帧来自指定本地端口的 mesh 数据。
 *
 * runtime 会把 frame.src -> port_id 记入 direct-table cluster，后续再向该 src
 * 发包时，就会自动通过该端口发出。
 */
int mesh_node_runtime_process_frame_on_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id,
    const uint8_t *frame_data,
    size_t frame_len);

/*
 * 从所有已绑定端口中轮询至多一帧并处理。
 *
 * 轮询策略为 round-robin：每次从 next_rx_port_index 开始扫描，谁先有完整帧就处理谁。
 */
int mesh_node_runtime_poll_once(struct mesh_node_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif