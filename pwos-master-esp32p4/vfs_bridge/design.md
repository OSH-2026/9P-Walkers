# vfs_bridge / cluster_host_vfs 设计说明

## 1. 模块定位

`cluster_host_vfs` 是主控侧的轻量级集群 VFS 桥接层。它不实现完整文件系统，而是把上层传入的全局路径：

```text
/mcu1/dev/temp
/mcu2/sys/health
```

解析成具体目标节点和远端本地路径，再通过 `mini9p_client` 发起文件操作。

当前版本已经接入 mesh runtime：

- 节点发现由 `mesh_host_runtime` + `cluster_config` 驱动；
- 节点命名由 `cluster_host_vfs` 按 UID 维护；
- 节点可达性由共享 `mesh cluster` 判定；
- 文件访问由 `cluster_host_vfs` 通过 `mini9p_client` 经 mesh 数据面完成。

## 2. 责任边界

`cluster_host_vfs` 负责：

- 维护 `UID <-> 节点名 <-> mini9P client` 的绑定表。
- 解析 `/mcuN/...` 路径。
- 选择目标节点和远端本地路径。
- 提供 `attach/detach/open/read/write/read_path/write_path/list/stat/close`。
- 维护本地 `fd -> route + remote_fid` 映射。
- 按共享 mesh cluster 的可达性结果刷新节点在线状态。

`cluster_host_vfs` 不负责：

- Mini9P 帧编解码（由 `mini9p_client` 负责）。
- 实际 UART/SPI/WiFi 收发（由 mesh transport 负责）。
- 从机本地文件树和驱动回调。
- 完整 POSIX 文件系统语义、并发锁、缓存、自动重连。

## 3. 核心数据结构

### 3.1 节点路由项

```c
struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];      // 面向用户的节点名，如 "mcu1"
    struct m9p_client *client;              // 通往该节点的 mini9P client
    uint8_t mesh_addr;                      // 当前会话的 mesh 短地址
    uint8_t hw_uid[CLUSTER_VFS_UID_LEN];    // 稳定硬件 UID
    enum cluster_vfs_m9p_state m9p_state;   // 9P 会话状态
    bool used;                              // 该路由项是否占用
};
```

状态含义：

```text
EMPTY     空路由项
NEW       已发现节点/已重连，但尚未 attach
ATTACHED  已完成 Mini9P attach，可用于 open/stat
```

> 注：早期版本曾有 READY/OFFLINE 状态，当前实现简化为 NEW/ATTACHED，离线通过 `used=false` 或 `m9p_state=NEW` 表达，具体以 `cluster_host_vfs.h` 为准。

### 3.2 打开文件表

```c
struct cluster_vfs_file {
    bool used;
    uint16_t local_fd;
    struct cluster_vfs_route *route;
    uint32_t remote_fid;
    uint64_t offset;
    uint8_t mode;
};
```

## 4. 关键设计原则

### 4.1 UID 是身份主键

- 不能把节点名绑定到 mesh 地址，因为地址可能重分配。
- 同一块硬件重连后，通常复用原来的 `mcuN` 名字。
- `cluster_vfs_discover_node()` 先按 UID 查历史映射，找不到再分配新名字。

### 4.2 mesh 地址是会话级

- 节点离线后，清除当前 `mesh_addr` 绑定，但保留 UID↔名字映射。
- 重新上线时通过 `discover_node()` 再次绑定新地址。

### 4.3 可达性来自 mesh cluster

- `cluster_host_vfs` 通过 `cluster_vfs_bind_mesh_cluster()` 绑定共享 cluster。
- 链路变化后调用 `cluster_vfs_refresh_all_nodes_from_cluster()` 批量刷新。
- 不可达节点会自动回退到 `NEW` 状态，不再允许新的文件访问。

## 5. 主要 API

详见 `cluster_host_vfs.h`。常用接口：

```c
int cluster_vfs_init(void);
int cluster_vfs_bind_mesh_cluster(struct cluster *mesh_cluster);

int cluster_vfs_discover_node(
    uint8_t mesh_addr,
    const uint8_t hw_uid[CLUSTER_VFS_UID_LEN],
    struct m9p_client *client,
    const char **out_target,
    bool *out_reused_mapping);

int cluster_vfs_refresh_node_from_cluster(uint8_t mesh_addr, bool *out_reachable);
int cluster_vfs_refresh_all_nodes_from_cluster(size_t *out_offline_count);

int cluster_vfs_attach(const char *target);
int cluster_vfs_detach(const char *target);

int cluster_vfs_open(const char *path, uint8_t mode, uint16_t *out_fd);
int cluster_vfs_read(uint16_t fd, uint8_t *buf, uint16_t *in_out_len);
int cluster_vfs_write(uint16_t fd, const uint8_t *data, uint16_t len, uint16_t *out_written);
int cluster_vfs_close(uint16_t fd);

int cluster_vfs_read_path(const char *path, uint8_t *buf, uint16_t *in_out_len);
int cluster_vfs_write_path(const char *path, const uint8_t *data, uint16_t len, uint16_t *out_written);
int cluster_vfs_stat(const char *path, struct m9p_stat *out_stat);
int cluster_vfs_list(const char *path, struct m9p_dirent *entries, size_t max_entries, size_t *out_count);
```

## 6. 与 cluster_config 的协作

```text
mesh_host_runtime 收到 REGISTER
        │
        ▼
cluster_config_on_mesh_node_registered()
        │
        ├──► cluster：更新拓扑/在线状态
        │
        └──► cluster_vfs_discover_node()：建立/复用 UID↔名字映射
        │
        ▼
cluster_vfs_attach("mcuN") 后，/mcuN/... 路径可用
```

链路变化时：

```text
mesh_host_runtime 收到 LINK_STATE / ROUTE_UPDATE
        │
        ▼
cluster 更新拓扑
        │
        ▼
cluster_config_refresh_all_nodes_connectivity()
        │
        ▼
cluster_vfs_refresh_all_nodes_from_cluster()
        │
        ▼
不可达节点回退到 NEW，已打开 fd 失效
```

## 7. 当前边界

- 不提供自动重连：节点离线后需要重新走 discover + attach 流程。
- 不提供目录递归 list 的缓存。
- 不处理并发：当前 `mesh_host_runtime_client_request` 是单事务模型。
- 主机侧尚未为“远端主动发到主机的 mini9P T*”挂接本地 server handler。
