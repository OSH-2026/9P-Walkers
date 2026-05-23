/**
 * @file mesh_host_runtime.h
 * @brief 主机侧 mesh runtime：把 raw UART、shared mesh processor、cluster_config 和 cluster_vfs 串成可运行闭环。
 *
 * 这个模块解决的是“接口都已经有了，但运行时谁来把它们真正接起来”的问题。
 *
 * 在当前代码库里，shared mesh 层已经提供了三块能力：
 * 1. mesh envelope：负责 REGISTER / LINK_STATE / MINI9P 等帧的编解码；
 * 2. mesh processor：负责收帧、判定本机/转发、把控制面或数据面分流给回调；
 * 3. mesh cluster：负责拓扑、链路与路由的维护。
 *
 * 主机侧 VFS 也已经具备：
 * 1. 按 UID 建立稳定 node_name；
 * 2. 节点离线时保留 UID <-> 名字映射；
 * 3. 把 9P attach 状态与“当前是否在线”分离。
 *
 * 缺失的恰恰是运行时胶水：
 * - 谁接收 REGISTER 并把 UID 送到 cluster_vfs？
 * - 谁在 LINK_STATE 变化后批量刷新所有节点的 reachability？
 * - 谁为每个已发现节点创建一个真正基于 mesh 数据面的 m9p_client？
 *
 * 本模块就是这层胶水。
 *
 * 关键职责：
 * 1. 以 mesh_processer 为核心，消费来自 raw transport 的 mesh 帧；
 * 2. 在 REGISTER 到来时，为 UID 分配/复用一个长期存在的 mesh-backed m9p_client；
 * 3. 调用 cluster_config_on_mesh_node_registered()，把 UID、名字映射和 client 绑定同步到 VFS；
 * 4. 在 LINK_STATE / ROUTE_UPDATE 变化后，调用 cluster_config_refresh_all_nodes_connectivity()，
 *    让所有已知节点按当前共享 cluster 的 reachability 统一回退或保持在线；
 * 5. 对外提供默认 UART 版本的初始化/启动入口，供 app_main 直接拉起后台轮询任务。
 *
 * 设计取舍：
 * - 当前实现明确采用“全局单事务”语义：同一时刻只允许一个 mesh-backed m9p_client
 *   在链路上等待自己的响应。这样可以在不引入复杂队列/调度器的前提下，先把真正
 *   可运行的发现链条和 VFS 访问链条打通。
 * - 若未来需要多并发请求，可在此基础上把 dispatch_busy 扩展成真正的 pending 表。
 */

#ifndef PWOS_MASTER_MESH_HOST_RUNTIME_H
#define PWOS_MASTER_MESH_HOST_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_client.h"
#include "../../pwos-shared/mesh/processer/mesh_processer.h"
#include "cluster_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 主机侧最多保留多少个“已发现节点的 mesh-backed client 槽位”。
 *
 * 这里直接与 shared cluster 的节点上限对齐，保证：
 * - cluster 最多认识多少个节点；
 * - runtime 最多就为多少个 UID 保留 client 对象。
 */
#define MESH_HOST_RUNTIME_MAX_CLIENTS CLUSTER_MAX_NODES

/* 未分配 mesh 地址时在 runtime client 槽位中使用的占位值。 */
#define MESH_HOST_RUNTIME_UNASSIGNED_ADDR MESH_ADDR_UNASSIGNED

/* 默认后台轮询任务参数，仅用于 ESP 平台默认实例。 */
#define MESH_HOST_RUNTIME_TASK_STACK_SIZE 4096
#define MESH_HOST_RUNTIME_TASK_PRIORITY 5
#define MESH_HOST_RUNTIME_IDLE_DELAY_MS 10u

struct mesh_host_runtime;

/**
 * @brief 每个 mesh-backed m9p_client 对应的 transport 上下文。
 *
 * m9p_client 的 transport_fn 是同步接口：
 * - 上层给它一帧 mini9P T*；
 * - 它必须在同一个调用里拿回对应 R*。
 *
 * 因此这里保存：
 * - runtime 指针：用于访问共享 processor、raw transport、cluster；
 * - slot_index：用于知道“当前是哪个 UID / mesh_addr 在发请求”。
 */
struct mesh_host_runtime_client_transport_ctx {
    struct mesh_host_runtime *runtime;
    size_t slot_index;
};

/**
 * @brief runtime 为某个已发现 UID 保留的长期 client 槽位。
 *
 * 该槽位的生命周期长于一次 mesh 地址绑定：
 * - 同一 UID 重新 REGISTER 时，优先复用原槽位；
 * - 地址被重分配时，只更新 mesh_addr，而不丢掉这个 client 对象；
 * - 节点暂时离线时，把 online 置 false，但保留 UID，以便后续重连复用。
 */
struct mesh_host_runtime_client_slot {
    bool used;
    bool online;
    uint8_t mesh_addr;
    uint8_t uid[MESH_UID_LEN];
    struct m9p_client client;
    struct mesh_host_runtime_client_transport_ctx transport_ctx;
};

/**
 * @brief runtime 初始化配置。
 *
 * 调用约束：
 * 1. mesh_cluster 必须来自 cluster_config_mesh_cluster()，因为 runtime 需要和
 *    cluster_config/cluster_vfs 共享同一份主机 cluster 真相；
 * 2. send_frame / receive_frame 必须是“原始整帧”的 transport；
 * 3. transport_ctx 会原样传给这两个回调，通常是 UART transport 实例。
 */
struct mesh_host_runtime_config {
    mesh_processer_send_frame_fn send_frame;
    mesh_processer_receive_frame_fn receive_frame;
    void *transport_ctx;
    struct cluster *mesh_cluster;
    uint8_t local_addr;
    uint8_t default_hop;
};

/**
 * @brief 主机侧 mesh runtime 对象。
 *
 * 该结构体适合：
 * - 默认全局单例；
 * - 或者主机侧测试代码里的栈上/静态实例。
 *
 * 字段说明：
 * - processor：真正执行收帧/分流/转发的 shared mesh processor；
 * - dispatch_busy：当前是否已有某个同步请求正在独占接收链路；
 * - next_mesh_seq：主机主动发出的 MINI9P 数据面帧使用的 seq 计数器；
 * - clients：UID -> client 的长期槽位表。
 */
struct mesh_host_runtime {
    struct mesh_host_runtime_config config;
    struct mesh_processer processor;
    bool initialized;
    bool dispatch_busy;
    uint16_t next_mesh_seq;
    struct mesh_host_runtime_client_slot clients[MESH_HOST_RUNTIME_MAX_CLIENTS];
};

/**
 * @brief 获取安全的默认配置模板。
 *
 * 默认值策略：
 * - local_addr 默认使用主机地址 0x00；
 * - default_hop 默认沿用 mesh_processer 的默认值；
 * - send/receive/transport_ctx/mesh_cluster 仍需调用方显式补齐。
 */
void mesh_host_runtime_get_default_config(struct mesh_host_runtime_config *out_config);

/**
 * @brief 初始化一个主机侧 mesh runtime 实例。
 *
 * 初始化会完成以下动作：
 * 1. 校验配置与 cluster_config 的共享 cluster 是否一致；
 * 2. 初始化 shared mesh processor，并挂上 host runtime 专用 control_handler；
 * 3. 清空所有 client 槽位，等待后续 REGISTER 动态填充。
 *
 * @param runtime 待初始化实例。
 * @param config 初始化配置。
 * @return 0 表示成功；负错误码表示配置非法或 processor 初始化失败。
 */
int mesh_host_runtime_init(
    struct mesh_host_runtime *runtime,
    const struct mesh_host_runtime_config *config);

/**
 * @brief 清空 runtime 状态。
 */
void mesh_host_runtime_deinit(struct mesh_host_runtime *runtime);

/**
 * @brief 处理一帧已经读到内存中的 raw mesh 数据。
 *
 * 这个接口主要供测试或自定义链路接入使用：
 * - 测试可直接构造 REGISTER / LINK_STATE / MINI9P 帧后注入；
 * - 若外层已有自己的收帧线程，也可在拿到完整帧后直接调用本函数。
 */
int mesh_host_runtime_process_frame(
    struct mesh_host_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len);

/**
 * @brief 从底层 transport 拉取一帧并处理。
 *
 * 该接口会尝试独占 runtime 的 dispatch 权：
 * - 若当前有某个同步 m9p_client 请求正在等待响应，则返回 busy；
 * - 否则从 raw transport 接收一帧，再交给 shared processor 分流。
 */
int mesh_host_runtime_poll_once(struct mesh_host_runtime *runtime);

/**
 * @brief 初始化默认全局 runtime 单例。
 *
 * 默认路径会自动完成：
 * 1. cluster_config_init_mesh_host()
 * 2. m9p_uart_transport_init_default()
 * 3. 基于默认 UART transport 装配 runtime
 */
int mesh_host_runtime_init_default(void);

/**
 * @brief 获取默认全局 runtime 单例。
 *
 * @return 已初始化时返回实例指针，否则返回 NULL。
 */
struct mesh_host_runtime *mesh_host_runtime_default(void);

/**
 * @brief 启动默认全局 runtime 的后台轮询任务。
 *
 * 在 ESP 平台上，该函数会创建一个 FreeRTOS 任务持续调用 poll_once；
 * 在主机单元测试环境中，该函数返回 -M9P_ERR_ENOTSUP，测试应直接手动调用
 * process_frame() 或 poll_once()。
 */
int mesh_host_runtime_start_default_task(void);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_MASTER_MESH_HOST_RUNTIME_H */