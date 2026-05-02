# vfs_bridge / cluster_vfs 设计说明

## 1. 模块定位

`cluster_vfs` 是集群统一命名空间的轻量级 VFS 桥接层。它不实现完整文件系统，而是把上层传入的全局路径：

```text
/mcu1/dev/temp
/mcu2/sys/health
```

解析成具体目标节点和远端本地路径，再通过 `mini9p_client` 发起文件操作。

直连场景示例：

```text
用户路径: /mcu1/dev/temp
路由目标: mcu1
远端路径: /dev/temp
```

设计目标：

- 上层 Shell/Lua/Web 只使用统一路径，不直接处理 Mini9P 的 fid/qid。
- `cluster_vfs` 保持纯 C 模块，不依赖 ESP-IDF、UART、WiFi、FreeRTOS 或动态内存。
- 底层通信差异由 `m9p_client` 的 transport 回调处理。
- 当前优先支持直连路由，保留静态一跳中继扩展方向。

## 2. 责任边界

`cluster_vfs` 负责：

- 维护目标节点路由表。
- 解析 `/mcuN/...` 路径。
- 选择下一跳和对应 `m9p_client`。
- 直连时把 `/mcuN/...` 转换成远端本地路径。
- 对上提供 `open/read/write/stat/close`。
- 维护本地 `fd -> route + remote_fid` 映射。

`cluster_vfs` 不负责：

- Mini9P 帧编解码。
- 实际 UART/SPI/WiFi 收发。
- 从机本地文件树和驱动回调。
- 动态路由、节点发现、最短路径。
- 完整 POSIX 文件系统语义。
- 并发锁、缓存、自动重连。

## 3. 核心数据结构

路由表记录当前节点如何到达目标节点：

```c
struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];
    char next_hop[CLUSTER_VFS_MAX_NAME];
    struct m9p_client *client;
    enum cluster_vfs_route_state state;
};
```

状态含义：

```text
EMPTY     空路由项
READY     已注册 route 和 client，但尚未 attach  **ps：这里的注册不验证节点存在，只是在路由表上记录**
ATTACHED  已完成 Mini9P attach，可用于 open/stat
OFFLINE   预留状态
```

打开文件表记录本地 fd 和远端 fid 的关系：

```c
struct cluster_vfs_file {
    bool used;
    uint16_t local_fd;
    struct cluster_vfs_route *route;
    uint16_t remote_fid;
    struct m9p_qid qid;//暂时没有消费，后续可能添加
    uint8_t mode;
    uint32_t offset;
};
```

`remote_fid` 是 Mini9P 会话内的临时文件句柄，不是全局文件 ID。`qid` 来自远端 `Ropen`，可供后续目录判断、缓存或调试使用。

## 4. 路径解析规则

`cluster_vfs` 只接受绝对集群路径：

```text
/mcuN/...
```

当前规则：

```text
/mcu1/dev/temp -> 命中 target=mcu1，直连时发送 /dev/temp
/mcu1          -> 命中 target=mcu1，直连时发送 /
/mcu1/         -> 命中 target=mcu1，直连时发送 /
/mcu10/dev     -> 不会误命中 target=mcu1
/               -> Master 本地虚拟根目录，当前支持 stat
mcu1/dev       -> 非绝对路径，返回 EINVAL
```

关键点：

- 匹配时跳过路径开头的 `/`，用 `mcu1/...` 与 route target 比较。
- target 后一个字符必须是字符串结束或 `/`，避免 `/mcu10` 误命中 `mcu1`。
- `resolve_path()` 只匹配 `ATTACHED` 状态路由。
- 直连路由 `target == next_hop` 时剥掉 `/mcuN` 前缀。
- 后续中继路由 `target != next_hop` 时，可保留全局路径发给下一跳 router。

## 5. 对外接口

当前头文件公开的核心接口：

```c
int cluster_vfs_init(void);

int cluster_vfs_add_direct(const char *target,
                           struct m9p_client *client);

int cluster_vfs_remove_route(const char *target);

int cluster_vfs_attach(const char *target);
int cluster_vfs_detach(const char *target);

int cluster_vfs_open(const char *path, uint8_t mode, uint16_t *out_fd);

int cluster_vfs_read(uint16_t fd,
                     uint8_t *buf,
                     uint16_t *in_out_len);

int cluster_vfs_write(uint16_t fd,
                      const uint8_t *data,
                      uint16_t len,
                      uint16_t *out_written);

int cluster_vfs_stat(const char *path,
                     struct m9p_stat *out_stat);

int cluster_vfs_close(uint16_t fd);
```

典型调用顺序：

```text
m9p_client_init
cluster_vfs_init
cluster_vfs_add_direct("mcu1", &client)
cluster_vfs_attach("mcu1")
cluster_vfs_open("/mcu1/dev/temp", M9P_OREAD, &fd)
cluster_vfs_read(fd, buf, &len)
cluster_vfs_close(fd)
```

头文件中保留了 `cluster_vfs_add_route()`，用于后续静态一跳中继；当前 `cluster_vfs.c` 尚未实现该函数。

## 6. 关键行为

### open

```text
resolve_path
-> alloc open file
-> m9p_client_open_path
-> 保存 route、remote_fid、qid、mode、offset
-> 返回 local_fd
```

### read/write

`read/write` 根据本地 fd 找到 `route + remote_fid`，调用对应 Mini9P client 接口。成功后按实际读写长度推进本地 offset。

权限判断按 Mini9P access mode 的低两位处理：

```text
M9P_OREAD  可读
M9P_OWRITE 可写
M9P_ORDWR  可读可写
```

不能用 `mode & M9P_OREAD` 判断只读权限，因为 `M9P_OREAD == 0`。

### stat

`cluster_vfs_stat("/")` 在本地合成 Master 虚拟根目录 stat，不访问远端。

普通路径：

```text
resolve_path
-> m9p_client_walk_path
-> m9p_client_stat
-> m9p_client_clunk
```

stat 使用临时 fid。即使远端 stat 返回错误，也要先 clunk 临时 fid，再返回原错误码。

### close

`close` 先释放本地 fd，再调用远端 `m9p_client_clunk()`。这样可以避免本地 fd 泄漏，同时把远端 clunk 错误返回给调用者。

## 7. 去中心化扩展方向

当前代码主要跑通直连路由：

```text
/mcu1/... -> target=mcu1,next_hop=mcu1 -> client_to_mcu1
```

后续中继路由可扩展为：

```text
/mcu3/... -> target=mcu3,next_hop=mcu1 -> client_to_mcu1
```

MVP 可先采用全局路径转发：

```text
用户路径: /mcu3/dev/temp
下一跳:   mcu1
发送路径: /mcu3/dev/temp
```

长期更干净的方案是在 Mini9P 外层增加 routing header：

```text
routing header: src_node, dst_node, session_id, ttl
payload:        原始 Mini9P frame
```

这样中间节点只看路由头并转发 payload，只有最终目标节点解析 Mini9P 和维护 fid 表。

## 8. 当前实现状态

已实现：

- `cluster_vfs_init`
- `cluster_vfs_add_direct`
- `cluster_vfs_remove_route`
- `cluster_vfs_attach / cluster_vfs_detach`
- `cluster_vfs_open`
- `cluster_vfs_read`
- `cluster_vfs_write`
- `cluster_vfs_read_path`
- `cluster_vfs_write_path`
- `cluster_vfs_list`
- `cluster_vfs_stat`
- `cluster_vfs_close`
- `cluster_vfs_get_route_state`

已覆盖的修复点：

- `/mcu1/...` 路径匹配不再被开头 `/` 卡住。
- `/mcu10/...` 不会误命中 `mcu1`。
- open 后保存 `file->qid`。
- stat 错误路径会 clunk 临时 fid。
- close 返回远端 clunk 错误码。
- 重复 target 返回 busy。
- remove route 防御空 route 指针。
- `/` 支持虚拟根目录 stat。
- `M9P_OREAD == 0` 的读权限判断已修正。
- 路径级 `read_path/write_path` 已封装 open/read-write/close。
- `read_path` 对目录返回 `EISDIR`，避免把目录项当普通文件内容。
- `list("/")` 会本地合成已注册节点挂载点。
- `list("/mcuN/...")` 会读取并解析远端目录项。
- `list` 对普通文件返回 `ENOTDIR`，避免把文件内容误解析为目录项。
- 可通过 `cluster_vfs_get_route_state()` 查询单个节点路由状态。
- remove route 和 detach 都会在仍有打开 fd 时返回 busy。

尚未实现或仍是规划：

- `cluster_vfs_add_route()` 的中继路由实现。
- `cluster_vfs_list_routes()` 这类批量路由枚举接口。
- routing header、多跳转发、动态路由。
- `OFFLINE` 状态的主动使用和自动重连策略。
- 自动重连、并发锁、目录缓存。

## 9. 测试状态

测试文件：

```text
pwos-master-esp32p4/vfs_bridge/test/test_main.c
```

测试方式：

- 使用帧级 `mock_transport()`。
- mock 会解码真实 `Tattach/Twalk/Topen/Tread/Twrite/Tstat/Tclunk` 请求。
- 再返回对应 `R*` 响应或 `Rerror`。
- 因此测试覆盖的是 `cluster_vfs -> mini9p_client -> transport` 的真实调用链。

当前覆盖：

- 重复路由。
- 根目录 stat。
- 路径边界匹配。
- open/read/write/stat/close。
- 路径级 read/write。
- 根目录和远端目录 list。
- 对目录 read_path 返回 `EISDIR`。
- 对普通文件 list 返回 `ENOTDIR`。
- 单节点 route state 查询。
- remove route / detach 在 fd 未关闭时返回 busy。
- stat 成功和失败路径的 clunk。
- close 返回远端 clunk 错误。

详细接口说明见：

```text
pwos-master-esp32p4/vfs_bridge/vfs接口说明.md
```

详细测试说明见：

```text
pwos-master-esp32p4/vfs_bridge/test/test说明.md
```
