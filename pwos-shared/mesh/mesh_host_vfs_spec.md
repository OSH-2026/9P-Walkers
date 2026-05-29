# Mesh 主机侧 VFS 接口说明

## 1. 文档目的

本文档专门说明主机侧如何把 mesh cluster 与 VFS 命名空间对接。

重点回答四个问题：

1. 新节点加入时，名字是怎么分配的。
2. 为什么必须传递硬件唯一序列号（UID）。
3. 节点离线时，为什么不能直接删掉名字映射。
4. VFS 的 9P 状态为什么要回退到 NEW（未 attach）。

## 2. 当前架构约定

主机侧不再维护一套独立于 mesh cluster 的旧 cluster 真相。

新的职责划分如下：

- mesh cluster：维护拓扑、链路、路由、可达性。
- cluster_config：负责把 mesh cluster 的事件翻译给 VFS。
- cluster_vfs：负责名字映射、UID 绑定、当前 mesh 地址绑定、9P attach 状态。

也就是说：

- 图是否连通，看 mesh cluster。
- 节点在 VFS 里叫什么，看 cluster_vfs。
- 两者怎么同步，看 cluster_config。

## 3. 为什么名字必须绑定 UID

不能把节点名绑定到 mesh 地址，原因有三点：

1. mesh 地址可能重分配。
2. 节点离线后再次加入时，地址可能变化。
3. 如果只按地址命名，同一块硬件重连后会被错误分配成新的 `mcuN`。

因此主机侧采用：

- `UID` 是节点身份主键。
- `node_name` 是面向用户的稳定别名。
- `mesh_addr` 只是当前一次接入会话的运行时地址。

## 4. 新节点发现流程

入口函数：

```c
cluster_config_on_node_discovered(mesh_addr, hw_uid, client, &node_name, &reused);
```

内部步骤：

1. cluster_config 把该节点加入 mesh cluster 的主机拓扑视图。
2. cluster_vfs 先按 `mesh_addr` 查当前活动绑定，处理地址复用冲突。
3. cluster_vfs 再按 `hw_uid` 查历史映射。
4. 若找到旧 UID，则复用旧名字。
5. 若没找到旧 UID，则自动生成新的 `mcuN` 名字。
6. 不论是首次发现还是重连，9P 状态都回到 `NEW`。

发现完成后，VFS 中该节点满足：

- `online = true`
- `m9p_state = NEW`

这里的 `NEW` 表示：

- 已经认识这个节点。
- 但还没有建立本轮 9P attach 会话。

## 5. attach 语义

节点被发现，不代表已经 attach。

attach 入口仍然是：

```c
cluster_vfs_attach(node_name);
```

attach 成功后：

- `m9p_state = ATTACHED`

之后 `/mcuN/...` 路径才允许被 `cluster_vfs_open()` 等接口解析并访问。

## 6. 节点离线流程

入口函数：

```c
bool reachable = false;
cluster_config_on_node_departed(mesh_addr, &reachable);
```

内部步骤：

1. cluster_config 调用 `cluster_mark_node_offline()`，把该节点从 mesh 图中摘掉。
2. cluster_config 再调用 `cluster_config_refresh_node_connectivity()`。
3. cluster_vfs 根据 cluster 的 reachability 结果，把节点回退到离线状态。
4. cluster_vfs 保留名字和 UID 映射，但清除当前 mesh 地址绑定。
5. cluster_vfs 把 9P 状态回退到 `NEW`。
6. 与该节点相关的本地打开 fd 会被直接作废。

离线完成后，VFS 中该节点满足：

- `online = false`
- `m9p_state = NEW`
- `mesh_addr = 0xFF`

## 7. 为什么离线后不删除映射

离线后如果直接删映射，会带来两个问题：

1. 同一块硬件重新上线时，用户会看到新名字，破坏稳定命名。
2. 上层若保存了 `/mcuN/...` 语义，会因为节点重连而失去连续性。

因此正确策略是：

- 保留 `UID <-> node_name` 映射。
- 清除“当前在线绑定”和“当前 9P 会话状态”。

这样同一块硬件下次回来时，可以继续使用旧名字，但必须重新 attach。

## 8. 链路变化但节点未必退出

有些场景只是某条边断开，并不能立刻断言节点彻底离线。

这种情况应该：

1. 先更新 mesh cluster 图。
2. 再调用：

```c
cluster_config_refresh_node_connectivity(mesh_addr, &reachable);
```

若 `reachable == true`：

- 说明图上仍有其他路径通往该节点。
- VFS 不应贸然把它踢回 OFFLINE。

若 `reachable == false`：

- 说明该节点当前对主机已不可达。
- VFS 应回退到 OFFLINE + NEW。

## 9. 关键接口清单

主机桥接入口：

- `cluster_config_init_mesh_host`
- `cluster_config_mesh_cluster`
- `cluster_config_on_node_discovered`
- `cluster_config_on_mesh_node_registered`
- `cluster_config_refresh_node_connectivity`
- `cluster_config_refresh_all_nodes_connectivity`
- `cluster_config_on_node_departed`

主机 service 入口：

- `mesh_host_service_init_default`
- `mesh_host_service_start_default_task`
- `mesh_host_service_default_runtime`

VFS 节点管理接口：

- `cluster_vfs_bind_mesh_cluster`
- `cluster_vfs_discover_node`
- `cluster_vfs_mark_node_offline`
- `cluster_vfs_refresh_node_from_cluster`
- `cluster_vfs_attach`
- `cluster_vfs_detach`

cluster 连通性接口：

- `cluster_mark_node_offline`
- `cluster_can_reach`
- `cluster_get_node_online`

## 10. 当前实现边界

当前实现已经覆盖：

1. 同 UID 重连复用旧名字。
2. 新 UID 自动分配新名字。
3. 节点离线后保留映射、回退 9P 状态。
4. cluster 图变化后按可达性刷新 VFS 状态。
5. runtime 已自动把广播 REGISTER 和 LINK_STATE 事件接入 cluster_config / cluster_vfs。
6. runtime 已为每个已发现 UID 绑定真实的 mesh-backed mini9P client。

当前实现尚未自动完成：

1. 默认 runtime 目前采用“同一时刻只允许一个同步 mini9P 请求占用接收链路”的串行模型，尚未扩展到多请求并发调度。
2. 主机侧目前仍未为“远端主动发到主机的 mini9P T*”挂接本地 server handler。

因此现阶段的正式约定是：

- 接口、状态机与 runtime 自动接线都已落地，并已通过主机侧回归测试。
- 若后续需要更高吞吐或双向主机服务能力，应在现有 runtime 之上继续扩展并发请求表与本地 mini9P server 接入。
