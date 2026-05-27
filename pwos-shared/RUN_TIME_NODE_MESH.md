# 主机侧节点 Mesh Runtime 概要

## 1. 文档目的

本文档只概括这套主机侧自动 runtime 链条，以及上层真正需要关心的对外接口。

本文刻意忽略以下透明实现细节：

- runtime 内部如何缓存 client。
- processor 内部如何分流和转发。
- 请求等待期间的内部调度细节。
- cluster / VFS 内部表项如何存储。

如果只想知道“系统怎么自动发现节点、对外该调用什么接口、文件访问怎么走”，看这一份即可。

## 2. 这套 runtime 链条解决什么问题

主机侧现在已经不是“静态写死 mcu1/mcu2”的模型，而是运行时自动建立节点视图。

这条链主要解决四件事：

1. 主机启动后，自动接收 mesh 控制帧。
2. 新节点发来 REGISTER 后，自动建立 UID 到节点名的稳定映射。
3. 节点链路变化后，自动刷新 VFS 中的在线状态与 9P 状态。
4. 上层通过 `/mcuN/...` 路径访问节点时，底层自动走 mesh 数据面，不需要上层自己拼 mesh 帧。

从调用者角度看，这套链条的核心价值是：

- 节点名字稳定。
- 节点发现自动化。
- 节点离线回退自动化。
- 文件访问接口保持不变。

## 3. 运行时总链条

### 3.1 启动阶段

主机启动时调用：

```c
mesh_host_runtime_start_default_task();
```

该入口会自动完成：

1. 初始化主机侧共享 mesh cluster。
2. 初始化 cluster_vfs。
3. 初始化默认 UART raw transport。
4. 启动后台 runtime 轮询任务。

也就是说，正常固件启动后，上层不需要再手动把 REGISTER、LINK_STATE 一条条喂给 VFS。

### 3.2 新节点发现阶段

当远端节点接入后，它会发 REGISTER。

在当前从机接线里，这个 REGISTER 不是靠上层业务手写触发，而是由单个
`mesh_node_runtime` 统一管理本节点全部 UART 后自动完成：

1. `mini9p_service_init()` 先初始化本地 `mini9p_server` 和 raw mesh UART transport。
2. 然后把所有本地串口绑定到同一个 `mesh_node_runtime`。
3. `mesh_node_runtime` 在 init 成功后，会向每个已绑定 UART 各发送一帧 REGISTER；若板级启用了 Wi-Fi，也会额外通过保留的 Wi-Fi 特殊端口发送一帧 REGISTER。
4. REGISTER 里会携带当前板子的稳定硬件 UID、该 runtime 汇总后的端口位图，以及 `wifi_supported` 标记；Wi-Fi 启用时，端口位图最高位保留给 Wi-Fi。

当前 STM32 实现里，8 字节 UID 由 96-bit 硬件唯一编号压缩得到：

- 前 4 字节直接取 `HAL_GetUIDw0()`。
- 后 4 字节取 `HAL_GetUIDw1() ^ HAL_GetUIDw2()`。

如果未来板级代码能够显式检测某条 UART 的 link-up 事件，还可以继续调用
`mini9p_service_notify_link_up()`、`mesh_node_runtime_notify_link_up()`，或者更细粒度的
`mesh_node_runtime_notify_link_up_on_port()`，在该事件上再次向对应串口重发 REGISTER。

多串口 runtime 额外承担两件事：

1. 子机侧 direct-table 路由表保存的是“dst -> 本地出口端口号”，而不是“dst -> 下一跳地址”。
2. 当某个邻居第一次从某个入端口出现时，runtime 会把这条 `src -> ingress_port` 关系写入 cluster，
   并向主机上报带 `local_port` 的 LINK_STATE，供主机全图推导子机路由表。

runtime 收到后，会自动完成：

1. 识别该节点当前的 mesh 地址。
2. 读取该节点硬件 UID。
3. 为该节点分配或复用稳定节点名。
4. 为该节点绑定一个真正可用的 mesh-backed mini9P client。
5. 若 REGISTER 声明该节点启用了 Wi-Fi，则主机把 Wi-Fi 能力写入节点信息，并补上一条 host<->node 的直连 Wi-Fi 拓扑边。
6. 把节点同步到 VFS。

最终效果是：

- 新 UID 会分配新的 `mcuN` 名字。
- 已知 UID 会复用旧名字。
- 节点进入 `READY` 状态，但尚未 attach。

### 3.3 链路变化阶段

当 mesh 中出现 LINK_STATE 或 ROUTE_UPDATE 变化时，runtime 会自动刷新可达性。

调用者真正可以依赖的结果只有两个：

1. 仍可达的节点继续保持在线。
2. 不可达的节点自动回退到 OFFLINE，并把 9P 状态回退到 NEW。

这意味着：

- 上层不需要自己维护一份“节点在线表”。
- 上层只需要在真正访问前判断节点当前状态，或者直接尝试 attach / open。

### 3.4 文件访问阶段

当节点已经被发现后，上层仍然使用 cluster_vfs 暴露的统一路径接口，例如：

```c
cluster_vfs_attach("mcu1");
cluster_vfs_read_path("/mcu1/sys/health", buf, &len);
cluster_vfs_write_path("/mcu1/dev/led", data, len, &written);
```

这里最重要的一点是：

- 上层看到的接口还是 VFS 路径接口。
- 底层是否走直连、是否走中继、如何封装 mesh MINI9P，都由 runtime 负责。

## 4. 调用者真正需要关心的状态语义

### 4.1 节点名字

节点名字不是按 mesh 地址绑定，而是按硬件 UID 绑定。

因此：

- 同一块硬件重连后，通常仍然是原来的 `mcuN`。
- mesh 地址变化不会让用户看到节点名字漂移。

### 4.2 节点在线状态

对外可理解为三种常见状态：

1. `READY`
   表示节点已经被 runtime 发现，当前在线，但还没有建立本轮 attach 会话。

2. `ATTACHED`
   表示节点已经 attach，可以正常通过 `/mcuN/...` 路径访问。

3. `OFFLINE`
   表示节点当前不可达，名字映射仍然保留，但本轮访问会话已经失效。

### 4.3 9P 状态回退

节点离线后，不会删除名字映射，而是回退到：

- 名字仍保留。
- UID 仍保留。
- 当前 mesh 地址解绑。
- 9P 状态回退到 `NEW`。

这样做的意义是：

- 节点下次回来时还能复用旧名字。
- 但必须重新 attach，避免沿用旧会话。

## 5. 对外接口分层

这套 runtime 对外接口可以按三层理解。

### 5.1 第一层：runtime 启动接口

这层用于“把自动链条拉起来”。

#### 默认启动入口

```c
int mesh_host_runtime_start_default_task(void);
```

用途：

- 固件正常启动时直接调用。
- 自动初始化默认 UART + cluster + VFS + runtime 后台任务。

建议：

- 正常运行时优先使用这个接口。

#### 默认实例初始化

```c
int mesh_host_runtime_init_default(void);
struct mesh_host_runtime *mesh_host_runtime_default(void);
```

用途：

- 需要先初始化，但暂不启动后台任务时使用。
- 需要拿默认 runtime 实例做额外测试或诊断时使用。

#### 自定义实例初始化

```c
void mesh_host_runtime_get_default_config(struct mesh_host_runtime_config *out_config);
int mesh_host_runtime_init(struct mesh_host_runtime *runtime,
                           const struct mesh_host_runtime_config *config);
int mesh_host_runtime_process_frame(struct mesh_host_runtime *runtime,
                                    const uint8_t *frame_data,
                                    size_t frame_len);
int mesh_host_runtime_poll_once(struct mesh_host_runtime *runtime);
void mesh_host_runtime_deinit(struct mesh_host_runtime *runtime);
```

用途：

- 主机侧单元测试。
- 非默认 transport 的接入。
- 手动驱动一帧一帧处理的调试场景。

如果不是在做测试或特殊接线，通常不需要直接碰这一组接口。

### 5.2 第二层：runtime 与 VFS 之间的桥接接口

这层用于“把 mesh 事件翻译成 VFS 可见状态”。

#### 初始化桥接上下文

```c
int cluster_config_init_mesh_host(void);
struct cluster *cluster_config_mesh_cluster(void);
```

用途：

- 初始化共享 mesh cluster 与 cluster_vfs。
- 为 runtime、测试和诊断代码提供同一份 cluster 真相。

#### 节点发现接口

```c
int cluster_config_on_mesh_node_registered(uint8_t mesh_addr,
                                           const uint8_t hw_uid[MESH_UID_LEN],
                                           struct m9p_client *client,
                                           const char **out_name,
                                           bool *out_reused_mapping);
```

用途：

- 真正 runtime 收到 REGISTER 时使用。
- 不额外伪造 host 直连边。
- 只负责把 UID、名字和 client 同步给 VFS。

#### 手工直连发现接口

```c
int cluster_config_on_node_discovered(uint8_t mesh_addr,
                                      const uint8_t hw_uid[MESH_UID_LEN],
                                      struct m9p_client *client,
                                      const char **out_name,
                                      bool *out_reused_mapping);
```

用途：

- 测试场景。
- 手工注入场景。
- 需要直接把某节点当作 host 直连节点注册时使用。

#### 连通性刷新接口

```c
int cluster_config_refresh_node_connectivity(uint8_t mesh_addr, bool *out_reachable);
int cluster_config_refresh_all_nodes_connectivity(size_t *out_offline_count);
```

用途：

- 单点检查某个节点当前是否仍可达。
- 在拓扑变化后批量刷新所有节点状态。

建议：

- 处理 LINK_STATE / ROUTE_UPDATE 后优先使用批量刷新接口。
- 单点接口更适合诊断或测试。

#### 节点离线接口

```c
int cluster_config_on_node_departed(uint8_t mesh_addr, bool *out_reachable);
```

用途：

- 明确确认某个节点已经退出时调用。
- 会把节点从图中摘掉，并同步回退 VFS 状态。

### 5.3 第三层：上层真正业务访问接口

这层就是上层模块最常用的接口。

#### 状态查询接口

节点在线状态、可达性和下一跳由 shared mesh cluster / `cluster_config_*`
查询。VFS 只负责按节点名发起 Mini9P 会话和文件访问，不再暴露节点信息快照。

#### 会话接口

```c
int cluster_vfs_attach(const char *target);
int cluster_vfs_detach(const char *target);
```

用途：

- 在访问 `/mcuN/...` 之前建立或回退 9P 会话。

#### 路径访问接口

```c
int cluster_vfs_open(const char *path, uint8_t mode, uint16_t *out_fd);
int cluster_vfs_read(uint16_t fd, uint8_t *buf, uint16_t *in_out_len);
int cluster_vfs_write(uint16_t fd, const uint8_t *data, uint16_t len, uint16_t *out_written);
int cluster_vfs_close(uint16_t fd);

int cluster_vfs_read_path(const char *path, uint8_t *buf, uint16_t *in_out_len);
int cluster_vfs_write_path(const char *path, const uint8_t *data, uint16_t len, uint16_t *out_written);
int cluster_vfs_list(const char *path, struct m9p_dirent *entries, size_t max_entries, size_t *out_count);
int cluster_vfs_stat(const char *path, struct m9p_stat *out_stat);
```

用途：

- 统一用 `/mcuN/...` 路径访问远端节点。
- 不关心节点是直连还是多跳。

## 6. 推荐使用方式

### 6.1 固件正常运行时

推荐顺序：

1. 启动时调用 `mesh_host_runtime_start_default_task()`。
2. 等待 runtime 自动发现节点。
3. 用 shared mesh cluster 或 `cluster_config_*` 查询节点是否在线/可达。
4. 对需要访问的节点执行 `cluster_vfs_attach("mcuN")`。
5. 通过 `cluster_vfs_read_path()` / `cluster_vfs_write_path()` / `cluster_vfs_list()` 访问。

### 6.2 测试或手工注入时

推荐顺序：

1. 调 `cluster_config_init_mesh_host()`。
2. 手工创建或提供 `m9p_client`。
3. 调 `cluster_config_on_node_discovered()` 或 `cluster_config_on_mesh_node_registered()`。
4. 再使用 `cluster_vfs_attach()` 与路径访问接口验证行为。

## 7. 上层不用关心什么

上层通常不需要关心以下内容：

- MINI9P 请求是如何被再次封装成 mesh 帧的。
- next_hop 是如何从 cluster 里算出来的。
- REGISTER 广播帧是如何被识别为本机控制帧的。
- 节点离线时 runtime 内部如何回收或复用 client。

这些都属于 runtime 透明机制，上层只需要依赖其结果：

- 节点被自动发现。
- 节点状态被自动刷新。
- `/mcuN/...` 路径始终是统一入口。

## 8. 当前边界

当前这套 runtime 链条已经可以正式承担：

1. 自动 REGISTER 发现。
2. 自动 UID 到名字映射。
3. 自动链路变化回退。
4. 基于 mesh 的真实 mini9P 访问。
5. 直连与多跳两类路径访问。

当前仍然保留的边界是：

1. 默认 runtime 采用串行同步请求模型，不是高并发调度器。
2. 主机侧尚未对“远端主动发到主机的 mini9P T*”提供完整本地服务端处理。

因此这份文档描述的是“当前可直接使用的外部行为”，不是未来扩展目标。
