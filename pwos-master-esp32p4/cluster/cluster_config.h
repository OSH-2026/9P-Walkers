#ifndef CLUSTER_CONFIG_H
#define CLUSTER_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "mini9p_client.h"
#include "cluster.h"

/**
 * @brief 初始化主机侧 mesh cluster + VFS 桥接上下文。
 *
 * 新版本不再在这里硬编码静态 mcu1/mcu2 节点，而是只完成：
 * 1. 初始化共享 mesh cluster（主机默认使用 TOPOLOGY 模式）。
 * 2. 初始化 cluster_vfs 并绑定该 mesh cluster。
 * 3. 等待后续 mesh 控制面把“新节点发现/节点离线”事件送进来。
 *
 * @return 0 表示初始化成功；负错误码表示初始化失败。
 */
int cluster_config_init_mesh_host(void);

/**
 * @brief 获取主机侧共享 mesh cluster 对象。
 *
 * 供 processor、控制面或测试代码拿到同一个 cluster 实例，避免再维护一套
 * 旧的 host-only cluster。
 *
 * @return 已初始化时返回 cluster 指针，否则返回 NULL。
 */
struct cluster *cluster_config_mesh_cluster(void);

/**
 * @brief 处理“发现新节点/旧节点重连”事件。
 *
 * 该函数会同时更新两层状态：
 * 1. mesh cluster：把节点加入主机拓扑视图；
 * 2. cluster_vfs：用 UID 建立/复用名字映射，并把 9P 状态置为 NEW。
 *
 * @param mesh_addr 当前节点 mesh 地址。
 * @param hw_uid 节点唯一硬件序列号。
 * @param client 用于访问该节点 mini9P 的 client。
 * @param out_name 输出最终节点名，可为 NULL。
 * @param out_reused_mapping 输出是否复用历史映射，可为 NULL。
 * @return 0 表示成功；负错误码表示 cluster 或 VFS 更新失败。
 */
int cluster_config_on_node_discovered(
	uint8_t mesh_addr,
	const uint8_t hw_uid[MESH_UID_LEN],
	struct m9p_client *client,
	const char **out_name,
	bool *out_reused_mapping);

/**
 * @brief 处理来自真正 mesh runtime 的 REGISTER 发现事件。
 *
 * 与 cluster_config_on_node_discovered() 的区别在于：
 * - 该接口假定 mesh cluster 已经由 runtime / processor 按真实控制面事件更新；
 * - 因此这里不会再额外伪造一条“host <-> node 直连边”；
 * - 它只负责把 REGISTER 中的 UID 信息同步给 VFS，并确保节点在线位可见。
 *
 * 适用场景：
 * - 主机通过 mesh_processer 收到 REGISTER；
 * - cluster_processor_control_handler 已经处理了 cluster 状态；
 * - 现在需要把 UID、名字映射和 mini9P client 绑定补进 cluster_vfs。
 *
 * @param mesh_addr 当前节点 mesh 地址。
 * @param hw_uid 节点唯一硬件序列号。
 * @param client 通往该节点的 mesh-backed mini9P client。
 * @param out_name 输出最终节点名，可为 NULL。
 * @param out_reused_mapping 输出是否复用历史映射，可为 NULL。
 * @return 0 表示成功；负错误码表示 cluster 或 VFS 更新失败。
 */
int cluster_config_on_mesh_node_registered(
	uint8_t mesh_addr,
	const uint8_t hw_uid[MESH_UID_LEN],
	struct m9p_client *client,
	const char **out_name,
	bool *out_reused_mapping);

/**
 * @brief 在拓扑变化后刷新某个节点在 VFS 中的连通状态。
 *
 * 适合由 LINK_STATE、路由重算或上层诊断逻辑调用：
 * - 仍可达：保持 VFS 节点在线。
 * - 不可达：VFS 自动回退到 NEW（未 attach）状态。
 *
 * @param mesh_addr 需要检查的节点地址。
 * @param out_reachable 输出当前是否仍可达。
 * @return 0 表示成功；负错误码表示 cluster 或 VFS 查询失败。
 */
int cluster_config_refresh_node_connectivity(uint8_t mesh_addr, bool *out_reachable);

/**
 * @brief 在拓扑发生变化后，批量刷新所有已知节点的 VFS 连通状态。
 *
 * 单个 LINK_STATE 变化可能影响的不只是消息源和其邻居，还可能让更远的
 * 下游节点整体失联。因此真正的 mesh runtime 在处理拓扑更新后，通常应调用
 * 该接口而不是只刷新单个地址。
 *
 * @param out_offline_count 输出本轮被回退为 OFFLINE 的节点个数，可为 NULL。
 * @return 0 表示成功；负错误码表示初始化、cluster 或 VFS 查询失败。
 */
int cluster_config_refresh_all_nodes_connectivity(size_t *out_offline_count);

/**
 * @brief 处理“节点已经离线/退出”的事件。
 *
 * 该函数会先在 mesh cluster 中摘掉该节点，再把 VFS 中对应节点的 9P 会话
 * 状态回退到 NEW，保留名字 <-> UID 映射供后续重连复用。
 *
 * @param mesh_addr 离线节点地址。
 * @param out_reachable 输出图更新后该节点是否仍被判定可达。
 * @return 0 表示成功；负错误码表示 cluster 或 VFS 更新失败。
 */
int cluster_config_on_node_departed(uint8_t mesh_addr, bool *out_reachable);

/* 兼容旧调用点：现在仅作为新初始化入口的别名。 */
int cluster_init_static_nodes(void);

#endif /* CLUSTER_CONFIG_H */
