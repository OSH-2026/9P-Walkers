# cluster_vfs 接口详解

本文档讲解 `pwos-master-esp32p4/vfs_bridge/cluster_vfs.h` 中的函数与数据结构，对应集群统一命名空间到 Mini9P 文件操作的 VFS 桥接层实现。


## 模块作用先览

`cluster_vfs` 位于上层 Shell/Lua/Web 与底层 `mini9p_client` 之间。上层使用统一路径：

```text
/mcu1/dev/temp
/mcu1/sys/health
```

`cluster_vfs` 负责解析第一段目标节点名，例如 `mcu1`，查找路由表，然后把直连路径转换成远端节点的本地路径：

```text
/mcu1/dev/temp -> /dev/temp
/mcu1/sys/health -> /sys/health
```

上层不需要直接管理 Mini9P 的 `fid`、`qid`、`Twalk`、`Topen`、`Tclunk` 等协议细节。

---

## 数据结构先览

### `enum cluster_vfs_route_state` 路由状态

保存路由表项当前所处状态：

- `CLUSTER_VFS_ROUTE_EMPTY`：空路由项。
- `CLUSTER_VFS_ROUTE_READY`：已注册 `target/client`，但尚未 attach。
- `CLUSTER_VFS_ROUTE_ATTACHED`：已完成 Mini9P attach，可以用于 open/stat。
- `CLUSTER_VFS_ROUTE_OFFLINE`：预留状态，当前实现未主动使用。

当前路径解析只匹配 `ATTACHED` 状态路由，因此访问 `/mcuN/...` 前必须先调用 `cluster_vfs_attach(target)`。

---

### `struct cluster_vfs_route` 路由表项

保存“目标节点如何到达”的信息：

```c
struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];
    char next_hop[CLUSTER_VFS_MAX_NAME];
    struct m9p_client *client;
    enum cluster_vfs_route_state state;
};
```

字段说明：

- `target`：最终目标节点名，例如 `"mcu1"`、`"mcu3"`，不带前导 `/`。
- `next_hop`：下一跳节点名。直连路由中 `target == next_hop`。
- `client`：通往下一跳的 Mini9P client，由上层提前初始化。
- `state`：当前路由状态。

直连示例：

```text
target=mcu1, next_hop=mcu1, client=client_to_mcu1
```

后续中继示例：

```text
target=mcu3, next_hop=mcu1, client=client_to_mcu1
```

---

### `struct cluster_vfs_file` 本地 fd 映射项

`cluster_vfs_open()` 返回的是本地 fd。该 fd 在 `cluster_vfs` 内部映射到远端 Mini9P fid：

```c
struct cluster_vfs_file {
    bool used;
    uint16_t local_fd;
    struct cluster_vfs_route *route;
    uint16_t remote_fid;
    struct m9p_qid qid;
    uint8_t mode;
    uint32_t offset;
};
```

字段说明：

- `used`：该 open 表项是否正在使用。
- `local_fd`：返回给上层的本地 fd，本质是 open 表数组下标。
- `route`：该 fd 使用的路由。
- `remote_fid`：远端 Mini9P 会话中的 fid。
- `qid`：远端 `Ropen` 返回的对象标识。
- `mode`：打开模式，如 `M9P_OREAD`、`M9P_OWRITE`、`M9P_ORDWR`。
- `offset`：本地维护的读写偏移。

`remote_fid` 只在对应 Mini9P 会话内有效，不是全局文件 ID。

---

## 路径规则先览

`cluster_vfs` 接收集群绝对路径：

```text
/mcuN/...
```

当前路径行为：

```text
/mcu1/dev/temp -> 命中 target=mcu1，直连时发送远端路径 /dev/temp
/mcu1          -> 命中 target=mcu1，直连时发送远端路径 /
/mcu1/         -> 命中 target=mcu1，直连时发送远端路径 /
/mcu10/dev     -> 不会误命中 target=mcu1
/               -> Master 本地虚拟根目录，当前支持 stat
mcu1/dev       -> 非绝对路径，返回 EINVAL
```

路径匹配时会跳过开头 `/`，并检查 target 边界：只有 `/mcu1` 或 `/mcu1/...` 可以匹配 `mcu1`，`/mcu10/...` 不会误匹配。

---

## 函数逐个讲解

### `cluster_vfs_init`

初始化 `cluster_vfs` 的全局静态状态。

作用：

- 清空路由表。
- 清空本地 open file 表。
- 不创建 `m9p_client`。
- 不初始化底层 transport。

返回：

```text
0 成功
```

---

### `cluster_vfs_add_direct`

添加一条直连路由。

```c
int cluster_vfs_add_direct(const char *target,
                           struct m9p_client *client);
```

参数：

- `target`：目标节点名，例如 `"mcu1"`，不包含 `/`。
- `client`：通往该节点的 Mini9P client，必须已由 `m9p_client_init()` 初始化。

行为：

- 设置 `target == next_hop`。
- 保存 `client` 指针。
- 初始状态为 `READY`。
- 若重复添加相同 target，返回 busy，避免出现不可达死路由。

返回：

```text
0                  成功
-M9P_ERR_EINVAL    target 或 client 为空
-M9P_ERR_EBUSY     target 重复，或路由表已满
```

---

### `cluster_vfs_remove_route`

删除一条目标节点路由。

```c
int cluster_vfs_remove_route(const char *target);
```

参数：

- `target`：要删除的目标节点名。

行为：

- 查找非空路由项。
- 如果该 route 仍被任何打开的 fd 使用，则返回 busy。
- 否则把 route 标记为 `EMPTY`。

返回：

```text
0                  成功
-M9P_ERR_EINVAL    target 为空
-M9P_ERR_EBUSY     仍有 fd 使用该路由
-M9P_ERR_ENOENT    未找到该 target
```

---

### `cluster_vfs_attach`

对目标路由执行 Mini9P attach。

```c
int cluster_vfs_attach(const char *target);
```

行为：

- 查找 `READY` 状态的 target。
- 调用对应 client 的 `m9p_client_attach()`。
- attach 成功后，把路由状态改为 `ATTACHED`。

返回：

```text
0                  成功
-M9P_ERR_EINVAL    target 为空
-M9P_ERR_ENOENT    未找到 READY 状态的目标路由
其他负错误码       m9p_client_attach 返回的错误
```

---

### `cluster_vfs_detach`

标记目标路由断开。

```c
int cluster_vfs_detach(const char *target);
```

当前实现：

- 查找 `ATTACHED` 状态的 target。
- 把状态改回 `READY`。
- 不主动关闭已有 fd。

调用者应先关闭该路由上的 fd，再 detach。

---

### `cluster_vfs_open`

打开集群统一路径下的文件或目录。

```c
int cluster_vfs_open(const char *path, uint8_t mode, uint16_t *out_fd);
```

参数：

- `path`：集群绝对路径，例如 `"/mcu1/dev/temp"`。
- `mode`：Mini9P 打开模式，例如 `M9P_OREAD`、`M9P_OWRITE`、`M9P_ORDWR`。
- `out_fd`：输出本地 fd。

流程：

```text
resolve_path
-> alloc open file
-> m9p_client_open_path
-> 保存 route、remote_fid、qid、mode、offset
-> 返回 local_fd
```

返回：

```text
0                  成功
-M9P_ERR_EINVAL    参数非法或路径非法
-M9P_ERR_ENOENT    无匹配路由
-M9P_ERR_EBUSY     本地 open 表已满
-M9P_ERR_EMSIZE    映射后的远端路径超过协议限制
其他负错误码       m9p_client_open_path 返回的错误
```

---

### `cluster_vfs_read`

从已打开的本地 fd 读取数据。

```c
int cluster_vfs_read(uint16_t fd,
                     uint8_t *buf,
                     uint16_t *in_out_len);
```

参数：

- `fd`：`cluster_vfs_open()` 返回的本地 fd。
- `buf`：输出缓冲区。
- `in_out_len`：输入期望读取长度，输出实际读取长度。

行为：

- 根据 fd 找到 `route + remote_fid`。
- 调用 `m9p_client_read()`。
- 成功后推进本地 offset。

模式要求：

```text
M9P_OREAD 或 M9P_ORDWR 可以读
M9P_OWRITE 不允许读
```

注意：`M9P_OREAD == 0`，所以读权限不能用简单的按位与判断。

---

### `cluster_vfs_write`

向已打开的本地 fd 写入数据。

```c
int cluster_vfs_write(uint16_t fd,
                      const uint8_t *data,
                      uint16_t len,
                      uint16_t *out_written);
```

参数：

- `fd`：`cluster_vfs_open()` 返回的本地 fd。
- `data`：待写入数据。
- `len`：期望写入长度。
- `out_written`：输出实际写入长度。

行为：

- 根据 fd 找到 `route + remote_fid`。
- 调用 `m9p_client_write()`。
- 成功后推进本地 offset。

模式要求：

```text
M9P_OWRITE 或 M9P_ORDWR 可以写
M9P_OREAD 不允许写
```

---

### `cluster_vfs_stat`

查询集群路径对应对象的属性。

```c
int cluster_vfs_stat(const char *path,
                     struct m9p_stat *out_stat);
```

根路径 `/`：

- 由 `cluster_vfs` 本地合成虚拟根目录 stat。
- 不访问远端节点。
- `qid.type = M9P_QID_DIR | M9P_QID_VIRTUAL`。
- `flags = M9P_STAT_DIR | M9P_STAT_VIRTUAL`。
- `name = "/"`。

普通路径 `/mcuN/...`：

```text
resolve_path
-> m9p_client_walk_path
-> m9p_client_stat
-> m9p_client_clunk
```

注意：

- stat 使用临时 fid。
- stat 成功后必须 clunk。
- stat 失败后也会先 clunk，再返回原错误码，避免服务端 fid 泄漏。

---

### `cluster_vfs_close`

关闭本地 fd，并释放远端 fid。

```c
int cluster_vfs_close(uint16_t fd);
```

行为：

- 检查 fd 是否有效。
- 先释放本地 open 表项。
- 调用 `m9p_client_clunk()` 释放远端 fid。
- 返回远端 clunk 的结果。

这样即使远端 clunk 失败，本地 fd 也不会泄漏；同时上层仍能收到远端错误码。

返回：

```text
0                  成功
-M9P_ERR_EINVAL    fd 超出本地 open 表范围
-M9P_ERR_EFID      fd 未打开或已经关闭
其他负错误码       m9p_client_clunk 返回的错误
```

---

## 当前预留但未实现的接口

头文件中预留了：

```c
int cluster_vfs_add_route(const char *target,
                          const char *next_hop,
                          struct m9p_client *client);
```

该接口用于后续静态一跳中继路由：

```text
target=mcu3, next_hop=mcu1
```

当前 `cluster_vfs.c` 尚未实现该函数，主流程暂时应使用 `cluster_vfs_add_direct()`。

设计文档中提到的路径级快捷接口和目录列表接口：

```text
cluster_vfs_read_path
cluster_vfs_write_path
cluster_vfs_list
```

当前也还没有落地为公开实现。

---

## 总结

```text
路由管理:
  init -> add_direct -> attach/detach -> remove_route

文件访问:
  open -> read/write -> close

属性查询:
  stat("/")              -> 本地合成虚拟根目录
  stat("/mcuN/...")      -> walk -> stat -> clunk

当前最常用流程:
  m9p_client_init
  -> cluster_vfs_init
  -> cluster_vfs_add_direct
  -> cluster_vfs_attach
  -> cluster_vfs_open
  -> cluster_vfs_read / cluster_vfs_write
  -> cluster_vfs_close
```

典型只读访问：

```c
struct m9p_client client;
uint16_t fd;
uint8_t buf[64];
uint16_t len = sizeof(buf);

m9p_client_init(&client, transport, ctx);

cluster_vfs_init();
cluster_vfs_add_direct("mcu1", &client);
cluster_vfs_attach("mcu1");

cluster_vfs_open("/mcu1/dev/temp", M9P_OREAD, &fd);
cluster_vfs_read(fd, buf, &len);
cluster_vfs_close(fd);
```
