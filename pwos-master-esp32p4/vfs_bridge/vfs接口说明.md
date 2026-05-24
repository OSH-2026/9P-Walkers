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

### `enum cluster_vfs_m9p_state` Mini9P 会话状态

保存 VFS 与最终目标节点之间的 Mini9P 会话状态：

- `CLUSTER_VFS_M9P_EMPTY`：空节点映射项。
- `CLUSTER_VFS_M9P_NEW`：已发现节点，但尚未 attach。
- `CLUSTER_VFS_M9P_ATTACHED`：已完成 Mini9P attach，可以用于 open/stat。

节点是否可达由 shared mesh cluster 查询；当前路径解析要求 `m9p_state == ATTACHED` 且节点仍可达。

---

### `struct cluster_vfs_route` 节点映射项

保存“目标名字/UID 如何绑定到 Mini9P 会话”的信息；下一跳和可达性由 `pwos-shared/mesh/cluster` 维护：

```c
struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];
    struct m9p_client *client;
    uint8_t mesh_addr;
    uint8_t hw_uid[CLUSTER_VFS_UID_LEN];
    bool has_hw_uid;
    enum cluster_vfs_m9p_state m9p_state;
};
```

字段说明：

- `target`：最终目标节点名，例如 `"mcu1"`、`"mcu3"`，不带前导 `/`。
- `client`：通往该节点的 Mini9P client，由上层提前初始化。
- `mesh_addr`：当前绑定的 mesh 地址。
- `hw_uid/has_hw_uid`：硬件 UID 映射，用于同一硬件重连时复用名字。
- `m9p_state`：Mini9P 会话状态。

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

### `cluster_vfs_discover_node`

处理 mesh 侧发现节点/旧节点重连事件。

```c
int cluster_vfs_discover_node(uint8_t mesh_addr,
                              const uint8_t hw_uid[CLUSTER_VFS_UID_LEN],
                              struct m9p_client *client,
                              const char **out_target,
                              bool *out_reused_mapping);
```

参数：

- `mesh_addr`：当前 mesh 地址。
- `hw_uid`：硬件唯一 ID。
- `client`：通往该节点的 Mini9P client，必须已由 `m9p_client_init()` 初始化。
- `out_target`：输出自动分配或复用的节点名。
- `out_reused_mapping`：输出是否复用历史 UID 映射。

行为：

- 以 UID 为主键复用或分配 `mcuN` 名称。
- 保存 `client` 指针和当前 `mesh_addr`。
- 9P 会话状态回到 `NEW`。

返回：

```text
0                  成功
-M9P_ERR_EINVAL    target 或 client 为空
-M9P_ERR_EBUSY     target 重复，或路由表已满
```

---

### `cluster_vfs_attach`

对目标节点执行 Mini9P attach。

```c
int cluster_vfs_attach(const char *target);
```

行为：

- 查找 `READY` 状态的 target。
- 调用以该 target 为最终目标的 client 执行 `m9p_client_attach()`。
- 若底层 mesh 需要多跳，中继节点只转发 mesh frame；最终 target 节点处理 Mini9P attach 并返回应答。
- attach 成功后，派生路由状态变为 `ATTACHED`。

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
- 如果该 route 仍被任何打开的 fd 使用，则返回 busy。
- 把状态改回 `READY`。

调用者应先关闭该路由上的 fd，再 detach；VFS 不会替调用者主动关闭已有 fd。

返回：

```text
0                  成功
-M9P_ERR_EINVAL    target 为空
-M9P_ERR_EBUSY     仍有 fd 使用该路由
-M9P_ERR_ENOENT    未找到 ATTACHED 状态的目标路由
```

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

### `cluster_vfs_read_path`

按路径读取普通文件内容。

```c
int cluster_vfs_read_path(const char *path,
                          uint8_t *buf,
                          uint16_t *in_out_len);
```

流程：

```text
cluster_vfs_open(path, M9P_OREAD)
-> 检查 qid.type，若目标是目录则返回 EISDIR
-> cluster_vfs_read
-> cluster_vfs_close
```

用途：

- 给 `cat`、Lua `read()`、Web API 这类上层接口使用。
- 上层不需要管理本地 fd 或远端 fid。

返回：

```text
0                  成功
-M9P_ERR_EISDIR    期望普通文件，但目标是目录
其他负错误码       open/read/close 返回的错误
```

---

### `cluster_vfs_write_path`

按路径写入普通文件内容。

```c
int cluster_vfs_write_path(const char *path,
                           const uint8_t *data,
                           uint16_t len,
                           uint16_t *out_written);
```

流程：

```text
cluster_vfs_open(path, M9P_OWRITE)
-> cluster_vfs_write
-> cluster_vfs_close
```

用途：

- 给 `echo > path`、Lua `write()`、Web API 这类上层接口使用。
- 上层不需要管理本地 fd 或远端 fid。

---

### `cluster_vfs_list`

枚举目录项。

```c
int cluster_vfs_list(const char *path,
                     struct m9p_dirent *entries,
                     size_t max_entries,
                     size_t *out_count);
```

根路径 `/`：

- 由 `cluster_vfs` 本地合成已注册节点的挂载点目录项。
- 不访问远端节点。
- 例如已注册 `mcu1` 时，`cluster_vfs_list("/")` 返回目录项 `mcu1`。

远端目录 `/mcuN/...`：

```text
cluster_vfs_open(path, M9P_OREAD)
-> 检查 qid.type，若目标不是目录则返回 ENOTDIR
-> m9p_client_read(dir_offset)
-> m9p_parse_dirents
-> dir_offset += 本次解析出的目录项数
-> 重复 read，直到 EOF 或 entries 填满
-> cluster_vfs_close
```

注意：

- 普通文件 offset 按字节推进。
- 目录读取 offset 按“已消费目录项数量”推进，这是 `docs/protocol_spec.md` 的目录读取语义。
- 因此目录列表不能直接复用 `cluster_vfs_read()` 的字节 offset 推进逻辑。

返回：

```text
0                  成功
-M9P_ERR_ENOTDIR   期望目录，但目标不是目录
-M9P_ERR_EIO       远端目录数据无法解析为 dirent
其他负错误码       open/read/close 返回的错误
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

后续可以增加批量节点枚举接口，用于 Web 拓扑或 `nodes` 调试命令：

```text
cluster_vfs_list_routes
```

节点在线/可达性通过 shared mesh cluster 或 `cluster_config_*` 查询；VFS 当前不暴露节点信息快照。批量枚举暂未实现。

---

## 总结

```text
节点管理:
  cluster_config_init_mesh_host -> cluster_config_on_node_discovered -> attach/detach

文件访问:
  open -> read/write -> close
  read_path / write_path

属性查询:
  stat("/")              -> 本地合成虚拟根目录
  stat("/mcuN/...")      -> walk -> stat -> clunk

目录枚举:
  list("/")              -> 本地合成挂载点目录项
  list("/mcuN/...")      -> open -> read dirents -> parse -> close

当前最常用流程:
  m9p_client_init
  -> cluster_config_init_mesh_host
  -> cluster_config_on_node_discovered
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
cluster_config_on_node_discovered(mesh_addr, hw_uid, &client, &name, &reused);
cluster_vfs_attach("mcu1");

cluster_vfs_open("/mcu1/dev/temp", M9P_OREAD, &fd);
cluster_vfs_read(fd, buf, &len);
cluster_vfs_close(fd);
```

典型路径级读取：

```c
uint8_t buf[64];
uint16_t len = sizeof(buf);

cluster_vfs_read_path("/mcu1/dev/temp", buf, &len);
```

典型目录枚举：

```c
struct m9p_dirent entries[8];
size_t count = 0;

cluster_vfs_list("/mcu1/dev", entries, 8, &count);
```
