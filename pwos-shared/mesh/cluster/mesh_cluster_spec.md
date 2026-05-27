# Cluster 接口文档（v1）

## 1. 角色定位

cluster 是 mesh 控制面里的“节点与路由管理层”。它不负责文件协议，不负责串口收发，也不负责解析 mini9P 负载。

它只回答三类问题：

1. 这个 mesh 地址是不是本机。
2. 如果不是本机，应该转发给哪个 next_hop。
3. 当收到控制面路由更新时，如何刷新本地路由状态。

## 2. 设计取舍

### 2.1 主机与子机的职责划分

推荐策略如下：

- 子机：维护简化路由表，直接回答 dst -> next_hop。
- 主机：维护更完整的拓扑信息，再从拓扑中派生路由表。

为什么这样分：

- 子机资源有限，不适合长期维护完整拓扑图。
- 主机资源更充足，适合做路径选择、故障重算和诊断。
- cluster_processer 只需要一个稳定的 route_lookup 接口，不关心路由是手工填的，还是由拓扑算出来的。

### 2.2 为什么不让主机只用路由表

只用路由表当然能跑，但它会损失两类信息：

- 你很难知道“为什么是这个 next_hop”，也不方便做故障重算。
- 当链路变化时，主机需要自己补一层外部逻辑来重新生成路由。

所以更合理的做法是：

- 主机保存拓扑。
- 主机从拓扑生成路由。
- 子机只保存已经生成好的路由。

这和当前 ADR 保持一致。

## 3. 工作模式

cluster 目前支持两种模式：

### 3.1 DIRECT_TABLE

适合：

- 子机。
- 简单中继。
- 已由主机算好下一跳的运行环境。

行为：

- 直接维护 dst -> next_hop 表项。
- lookup 时线性扫描路由表。

### 3.2 TOPOLOGY

适合：

- 主机。
- 需要保留链路关系和做路径计算的场景。

行为：

- 维护拓扑链路。
- 必要时根据 local_addr 重新派生路由表。
- lookup 时优先查派生后的路由结果。

## 4. 数据模型

### 4.1 节点信息

`cluster_node` 目前只保留：

- addr：节点地址。
- online：在线状态。
- valid：是否占用。

说明：

- 这是最小可用模型。
- 将来可以再扩展 UID、节点名、能力位图等字段。
- 当前主机侧“节点名 <-> UID 映射”不放在 cluster 内，而放在 VFS 桥接层；
	cluster 只维护拓扑与可达性真相。

### 4.2 路由信息

`cluster_route` 目前保留：

- dst：目标地址。
- next_hop：下一跳地址。
- metric：路径代价。
- local：是否就是本机。
- valid：是否占用。

### 4.3 拓扑链路

`cluster_link` 目前保留：

- from / to：链路两端。
- metric：链路代价。
- bidirectional：是否双向。
- valid：是否占用。

## 5. API 说明

### 5.1 `cluster_get_default_config()`

作用：生成默认配置。

默认值：

- local_addr = 0xFF。
- mode = DIRECT_TABLE。

### 5.2 `cluster_init()`

作用：初始化 cluster 对象。

要求：

- cluster 指针不能为空。
- config 可为空，空时使用默认配置。

初始化后：

- 路由表清空。
- 拓扑表清空。
- 标记为 initialized。

### 5.3 `cluster_deinit()`

作用：清空整个对象。

适用场景：

- 节点断链重建。
- 上层销毁 cluster。

### 5.4 `cluster_set_local_addr()`

作用：更新本机地址。

使用场景：

- 节点重新接入后地址变更。
- 主机重新绑定某个本机身份。

### 5.5 `cluster_set_mode()`

作用：切换工作模式。

注意：

- 切到 TOPOLOGY 后，路由会被标记为脏，需要重新派生。
- 切到 DIRECT_TABLE 后，cluster 不再自动计算拓扑路由。

### 5.6 `cluster_set_node_online()`

作用：标记节点在线或离线。

说明：

- 这个接口只更新状态，不改变路由本身。
- 更适合控制面诊断和上层状态显示。

### 5.7 `cluster_add_route()`

作用：插入或更新一条静态路由。

适合：

- 子机直接配置表项。
- 主机临时塞兜底路由。
- 控制面收到 ROUTE_UPDATE 后做落地。

### 5.8 `cluster_remove_route()`

作用：删除一条静态路由。

### 5.9 `cluster_add_link()`

作用：添加一条拓扑链路。

适合：

- 主机记录全图拓扑。
- 链路发现或链路状态变化后更新图。

行为：

- 添加后会把路由标记为脏。
- 若 bidirectional = true，会视为双向链路。

### 5.10 `cluster_remove_link()`

作用：删除一条拓扑链路。

### 5.11 `cluster_mark_node_offline()`

作用：

- 把某个节点标记为离线。
- 清理与该节点相关的链路。
- 清理依赖该节点的静态路由。

适合：

- 主机确认某个节点已经退出 mesh。
- 需要把该节点从图中摘掉，再让上层决定是否回退 VFS 状态。

### 5.12 `cluster_rebuild_routes()`

作用：从拓扑重算路由表。

说明：

- 仅在 TOPOLOGY 模式下有意义。
- 当前实现使用静态数组上的最短路派生。
- 节点规模小的情况下，这种实现足够简单且可控。

### 5.13 `cluster_lookup_next_hop()`

这是给 mesh processor 用的核心接口。

输入：

- dst：目标 mesh 地址。

输出：

- out_is_local = true：目标就是本机。
- out_is_local = false：需要转发到 out_next_hop。

典型用法：

- processor 收到 mesh 帧后，先调用它。
- 如果目标不是本机，就按 next_hop 转发。
- 如果目标是本机，就把帧送到控制面或 mini9P 分支。

### 5.14 `cluster_can_reach()`

作用：只回答“某个地址当前还通不通”。

适合：

- 主机侧链路变化后，快速决定 VFS 是否应保留该节点在线状态。
- 调试时做 reachability 检查，而不是关心 next_hop 细节。

### 5.15 `cluster_get_node_online()`

作用：查询某个节点当前 online 标记。

说明：

- 该接口只返回控制面记录的在线标记。
- 它与 `cluster_can_reach()` 不完全等价：
	- online 更偏“控制面观测到的状态”。
	- can_reach 更偏“当前图和路由推导后的可达性结果”。

### 5.16 `cluster_apply_route_update()`

作用：处理 ROUTE_UPDATE 控制帧。

当前支持：

- MESH_ROUTE_SET：插入或更新静态路由。
- MESH_ROUTE_DELETE：删除静态路由。

补充说明：

- 在普通 DIRECT_TABLE / TOPOLOGY 路由表里，ROUTE_UPDATE.next_hop 通常仍表示下一跳节点地址。
- 当 cluster 配置为 `direct_routes_use_port_selectors=true` 时，ROUTE_UPDATE.next_hop 改为本地出口端口号。

这让控制面 handler 可以直接把负载交给 cluster，不必在别处重复写路由落地逻辑。

## 6. 错误语义

返回值约定：

- 0：成功。
- 负值：失败。

当前常见错误：

- MESH_ERR_BAD_FRAME：参数或对象不合法。
- MESH_ERR_INVALID_STATE：cluster 未初始化。
- MESH_ERR_NO_ROUTE：没有可用路由。
- MESH_ERR_BUSY：静态数组已满，无法再放入新节点/路由/链路。

## 7. 与主机侧 VFS 的配合

### 7.1 统一原则

主机侧不再维护一套独立于 mesh cluster 的“旧 cluster 真相”。

当前约定是：

- `pwos-shared/mesh/cluster` 是主机唯一的拓扑/可达性来源。
- 主机侧 `cluster_config` 只是桥接器，不再自己保存另一份图。
- `cluster_vfs` 只维护：
  - 节点名 <-> 硬件 UID 映射
  - 当前 mesh 地址绑定
  - 9P attach 状态

### 7.2 推荐桥接流程

1. 新节点加入：
	- `cluster_config_on_node_discovered()`
	- cluster 更新拓扑
	- VFS 用 UID 分配/复用名字

2. 节点离线：
	- `cluster_config_on_node_departed()`
	- cluster 摘图
	- VFS 把 9P 状态回退到 NEW，保留 UID 映射

3. 链路变化但未确认节点退出：
	- 先更新 cluster 图
	- 再调用 `cluster_config_refresh_node_connectivity()`
	- 由 `cluster_can_reach()` 决定 VFS 是否回退到离线/NEW

## 8. 与 mesh processor 的配合

mesh processor 不直接访问 cluster 内部字段，只通过 `route_lookup` 回调问 cluster：

1. 目的地址是不是本机。
2. 如果不是本机，下一跳是谁。

因此，cluster 的职责只要稳定满足这个查询即可，内部是静态路由表还是拓扑派生表，processor 都不关心。

## 9. 接入建议

### 子机建议

- 使用 DIRECT_TABLE。
- 维护简化路由表；若单 runtime 管理多个串口，建议开启端口选择器语义，让表项保存 `dst -> port_id`。
- 从控制面收到 ROUTE_UPDATE 时更新表项。

### 主机建议

- 使用 TOPOLOGY。
- 维护链路和节点在线状态。
- 通过 `cluster_rebuild_routes()` 生成可供 processor 查询的路由表。

## 10. 最小接线方式

通常可以这样接：

1. 初始化 cluster。
2. 配置本机地址和工作模式。
3. 填写静态路由或拓扑链路。
4. 在 mesh processor 的 `route_lookup` 回调里调用 `cluster_lookup_next_hop()`。

这样 processor 就能只依赖一个查询接口，而不需要知道路由是怎么来的。

## 11. 与 processor 的接口对齐

为减少重复封装，cluster 已提供两组与 processor 完全同签名的适配函数：

- cluster_processor_route_lookup
- cluster_processor_control_handler

这意味着你可以直接把它们挂进 processor 配置，不需要再写中间胶水。

示例：

```c
struct cluster g_cluster;
struct cluster_config cluster_cfg;
struct mesh_processer g_processor;
struct mesh_processer_config processor_cfg;

cluster_get_default_config(&cluster_cfg);
cluster_cfg.local_addr = 0x03;
cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE; /* 子机常用 */
cluster_init(&g_cluster, &cluster_cfg);

mesh_processer_get_default_config(&processor_cfg);
processor_cfg.local_addr = cluster_cfg.local_addr;
processor_cfg.send_frame = uart_or_link_send;
processor_cfg.receive_frame = uart_or_link_receive;
processor_cfg.transport_ctx = uart_ctx;

/* 直接对齐 cluster 适配器 */
processor_cfg.cluster_ctx = &g_cluster;
processor_cfg.route_lookup = cluster_processor_route_lookup;
processor_cfg.control_handler = cluster_processor_control_handler;

/* 数据面继续按你现有 mini9P 组装 */
processor_cfg.mini9p_server_handler = m9p_server_handle_frame;
processor_cfg.mini9p_server_ctx = &g_server;
processor_cfg.mini9p_client_handler = my_client_response_handler;
processor_cfg.mini9p_client_ctx = &g_client_mux;

mesh_processer_init(&g_processor, &processor_cfg);
```

## 12. 当前控制面适配器行为

cluster_processor_control_handler 当前默认语义：

1. REGISTER：标记源节点在线。
2. ASSIGN：当命中本机（或本机尚未分配地址）时更新 local_addr。
3. ROUTE_UPDATE：落地为静态路由表更新。
4. LINK_STATE：链路 up 时添加拓扑边，down 时删除拓扑边。
	当 LINK_STATE 带有 `local_port` 时，拓扑边还会记录“src 走向 neighbor 时应使用哪个本地端口”。
5. PING：自动回 PONG，形成最小探活闭环。
6. PONG/TIME_SYNC：标记源节点在线。

如果你后续需要更细粒度控制（鉴权、限流、租约校验），可以在这个适配器基础上继续增强。
