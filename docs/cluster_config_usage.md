# cluster_config 使用说明

## 1. 定位

`cluster_config` 是主机侧的**胶水层**，负责把 `mesh_host_runtime` 产生的 mesh 控制面事件翻译成 `cluster_host_vfs` 可见的节点状态。

源码：

- `pwos-master-esp32p4/cluster/cluster_config.c`
- `pwos-master-esp32p4/cluster/cluster_config.h`

它连接三层对象：

- `mesh_host_runtime`：发现节点、处理 LINK_STATE、下发 ROUTE_UPDATE。
- `cluster`（`pwos-shared/mesh/cluster`）：维护拓扑、链路、路由、可达性。
- `cluster_host_vfs`（`pwos-master-esp32p4/vfs_bridge`）：维护 `UID <-> 节点名` 映射、attach 状态、9P 会话。

## 2. 初始化

```c
cluster_config_init_mesh_host();
```

内部完成（`cluster_config.c:20`）：

1. `cluster_init()`：初始化主机侧共享 `cluster`，模式为 `CLUSTER_MODE_TOPOLOGY`，本机地址 `0x00`。
2. `cluster_vfs_init()`：初始化 VFS 桥接层。
3. `cluster_vfs_bind_mesh_cluster()`：把 VFS 绑定到同一个 cluster 实例。

`mesh_host_service_init()`（`mesh_host_service.c:134`）会调用该函数；正常固件启动时无需手动调用。

`cluster_config_mesh_cluster()` 返回同一个共享 cluster 指针，供 runtime 和测试代码使用。

## 3. 节点发现接口

### 3.1 来自真实 mesh runtime 的 REGISTER

```c
int cluster_config_on_mesh_node_registered(
    uint8_t mesh_addr,
    const uint8_t hw_uid[MESH_UID_LEN],
    struct m9p_client *client,
    const char **out_name,
    bool *out_reused_mapping);
```

使用场景：

- 主机通过 `mesh_processer` 收到 REGISTER。
- `cluster_processor_control_handler` 已处理 cluster 状态。
- 现在需要把 UID、名字映射和 `mini9P client` 绑定补进 `cluster_vfs`。

行为：

1. 调用 `cluster_set_node_online(mesh_addr, true)`。
2. 调用 `cluster_vfs_discover_node()`：按 UID 复用旧名字或分配新 `mcuN`，并建立 client 绑定。
3. **不**额外伪造 host 直连边（与 `on_node_discovered` 的区别）。

### 3.2 手工直连发现

```c
int cluster_config_on_node_discovered(
    uint8_t mesh_addr,
    const uint8_t hw_uid[MESH_UID_LEN],
    struct m9p_client *client,
    const char **out_name,
    bool *out_reused_mapping);
```

使用场景：

- 测试或手工注入。
- 需要把某节点直接视为 host 直连节点。

行为：在 `on_mesh_node_registered` 基础上，额外调用 `cluster_add_link(host, mesh_addr, metric, true)` 注入一条 host 直连边。

## 4. 连通性刷新

### 4.1 单节点刷新

```c
int cluster_config_refresh_node_connectivity(uint8_t mesh_addr, bool *out_reachable);
```

流程：

1. 调用 `cluster_vfs_refresh_node_from_cluster(mesh_addr, out_reachable)`。
2. 若 `reachable == true`：保持节点在线。
3. 若 `reachable == false`：VFS 中该节点回退到 `NEW`（未 attach），清除当前 `mesh_addr` 绑定，但保留 `UID <-> 名字` 映射。

### 4.2 批量刷新

```c
int cluster_config_refresh_all_nodes_connectivity(size_t *out_offline_count);
```

流程：

1. 调用 `cluster_vfs_refresh_all_nodes_from_cluster(out_offline_count)`。
2. 枚举所有已知节点，按 cluster 可达性统一刷新在线状态。

推荐在以下事件后调用：

- 收到 `LINK_STATE`
- 收到 `ROUTE_UPDATE`
- 路由重算后

## 5. 节点离线

```c
int cluster_config_on_node_departed(uint8_t mesh_addr, bool *out_reachable);
```

流程：

1. `cluster_mark_node_offline(mesh_addr)`：把节点从 mesh 图中摘掉。
2. `cluster_config_refresh_node_connectivity(mesh_addr, &reachable)`：刷新 VFS 状态。
3. VFS 中名字和 UID 映射保留，9P 状态回退到 `NEW`。

## 6. 典型调用时序

### 6.1 新节点上线

```text
mesh_host_runtime 收到 bootstrap REGISTER(src=0xFF)
    │
    ▼
分配/复用地址，构造 ASSIGN
    │
    ▼
ASSIGN 发出后调用 cluster_config_on_mesh_node_registered()
    │
    ├──► cluster_set_node_online(addr, true)
    └──► cluster_vfs_discover_node(addr, uid, client, ...)
                │
                ▼
        生成/复用 mcuN 名字，绑定 client
```

### 6.2 LINK_STATE 变化

```text
mesh_host_runtime 收到 LINK_STATE(src=A, neighbor=B)
    │
    ▼
cluster_processor_control_handler 更新 topology
    │
    ▼
cluster_config_refresh_all_nodes_connectivity()
    │
    ├──► 对仍可达节点：保持 online
    └──► 对不可达节点：VFS 回退到 NEW
    │
    ▼
mesh_host_runtime_sync_slots_from_cluster()
    │
    ▼
mesh_host_runtime_sync_controller_routes() 下发 ROUTE_UPDATE
```

### 6.3 节点离线

```text
外部诊断/超时逻辑调用 cluster_config_on_node_departed(addr)
    │
    ▼
cluster_mark_node_offline(addr)
    │
    ▼
cluster_config_refresh_node_connectivity(addr, &reachable)
    │
    ▼
cluster_vfs 回退 9P 状态，保留 UID/名字映射
```

## 7. 关键接口清单

| 接口 | 用途 |
|------|------|
| `cluster_config_init_mesh_host()` | 初始化 cluster + VFS 绑定。 |
| `cluster_config_mesh_cluster()` | 获取共享 cluster 指针。 |
| `cluster_config_on_mesh_node_registered()` | runtime 路径的节点发现。 |
| `cluster_config_on_node_discovered()` | 手工/测试路径的节点发现（额外注入直连边）。 |
| `cluster_config_refresh_node_connectivity()` | 单节点连通性刷新。 |
| `cluster_config_refresh_all_nodes_connectivity()` | 批量刷新所有节点。 |
| `cluster_config_on_node_departed()` | 节点离线处理。 |

## 8. 相关文件

- `pwos-master-esp32p4/cluster/cluster_config.c/.h`
- `pwos-master-esp32p4/vfs_bridge/cluster_host_vfs.c/.h`
- `pwos-shared/mesh/cluster/cluster.c/.h`
- `pwos-shared/RUN_TIME_NODE_MESH.md`：主机侧 runtime 调用指南。
- `pwos-shared/mesh/mesh_host_vfs_spec.md`：VFS 状态机说明。
