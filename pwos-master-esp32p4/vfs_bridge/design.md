# vfs_bridge / cluster_vfs 去中心化优先实现计划

## 1. 模块定位

`vfs_bridge` 是集群节点上的轻量级 VFS/router 桥接层。初代实现不只面向中心式 Master，也要优先支持去中心化的基本形态：任意具备转发能力的节点都可以通过同一套路由表把全局路径转发到下一跳。

它不实现完整文件系统内核，而是负责把用户侧统一路径映射到具体目标节点上的 Mini9P 文件操作。

目标路径示例：

```text
/mcu1/sys/health
/mcu1/dev/temp
/mcu2/compute/jacobi/result
```

桥接后的远端路径示例：

```text
/sys/health
/dev/temp
/compute/jacobi/result
```

也就是说，`/mcuN` 这一层由 `cluster_vfs` 的路由表负责；`/sys`、`/dev`、`/compute` 等节点由目标节点的本地虚拟文件树负责。

建议保持 `cluster_vfs` 为纯 C 模块，不直接依赖 ESP-IDF、UART、WiFi、FreeRTOS 或动态内存。底层通信差异由 `m9p_client` 的 transport 回调处理。

初代优先实现两类静态路由：

```text
/mcu1/... -> next_hop=mcu1 -> client_to_mcu1
/mcu3/... -> target=mcu3, next_hop=mcu1 -> client_to_mcu1
```

第一条是直连路由，第二条是静态一跳中继路由。每个节点只决定“下一跳是谁”，下下跳由下一个节点自己的 `cluster_vfs/router` 判断。

## 2. 设计边界

`cluster_vfs` 负责：

- 维护节点路由表，例如 `target=mcu3 -> next_hop=mcu1 -> client_to_mcu1`。
- 解析 `/mcuN/...` 路径，识别目标节点。
- 查路由表，选择下一跳和对应 `m9p_client`。
- 将集群路径转换成从机本地路径。
- 对上提供 `open/read/write/stat/close` 或路径级快捷接口。
- 对下调用 `m9p_client_open_path/read/write/stat/clunk`。
- 维护 Master 本地 fd 到远端 fid 的映射。

`cluster_vfs` 不负责：

- Mini9P 帧编解码。
- UART/SPI/WiFi 实际收发。
- 从机文件节点回调。
- 从机驱动控制。
- 完整 POSIX 文件系统语义。
- 动态路由协议。
- 复杂拓扑发现。
- 路由最短路径计算。
- 超过静态一跳之外的多跳路径维护。

## 3. 核心数据结构

### 3.1 路由表

路由表记录当前节点如何到达某个目标节点。初代支持直连路由和静态一跳中继路由，但不做动态路由协议。

```c
#define CLUSTER_VFS_MAX_ROUTES 8
#define CLUSTER_VFS_MAX_NAME   16

enum cluster_vfs_route_state {
    CLUSTER_VFS_ROUTE_EMPTY = 0,
    CLUSTER_VFS_ROUTE_READY,
    CLUSTER_VFS_ROUTE_ATTACHED,
    CLUSTER_VFS_ROUTE_OFFLINE,
};

struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];     /* 最终目标节点，如 "mcu3" */
    char next_hop[CLUSTER_VFS_MAX_NAME];   /* 下一跳节点，如 "mcu1" */
    struct m9p_client *client;             /* 通往 next_hop 的 Mini9P client */
    enum cluster_vfs_route_state state;
};
```

初代版本使用固定大小数组：

```text
/mcu1 -> route(target=mcu1, next_hop=mcu1, client=client_to_mcu1)
/mcu2 -> route(target=mcu2, next_hop=mcu2, client=client_to_mcu2)
```

后续去中心化路由示例：

```text
/mcu3 -> route(target=mcu3, next_hop=mcu1, client=client_to_mcu1)
```

此时本节点并不直连 `mcu3`，但知道应把请求交给 `mcu1` 转发。

### 3.2 fd 到 fid 的映射表

`cluster_vfs_open()` 返回 Master 本地 fd。这个 fd 在 `cluster_vfs` 内部映射到某个从机上的远端 fid。

```c
#define CLUSTER_VFS_MAX_OPEN 16

struct cluster_vfs_file {
    bool used;
    uint16_t local_fd;
    struct cluster_vfs_route *route;
    uint16_t remote_fid;
    uint8_t mode;
    uint32_t offset;
};
```

注意：初代直连时，`remote_fid` 是目标从机上的 fid。后续中继模式下，`remote_fid` 可能只是下一跳 router 会话内的 fid，最终目标 fid 由中继节点维护。这个区别由 router 节点处理，上层 Shell/Lua/Web 不感知。

### 3.3 连接管理

初代版本只维护简单状态：

```text
EMPTY     空路由项
READY     已注册 route 和 client，但不保证 attach 成功
ATTACHED  对 next_hop 已完成 Mini9P attach
OFFLINE   最近访问下一跳失败，可由上层重新 attach
```

连接恢复策略第一版可以很保守：

- `cluster_vfs_attach(target)` 显式 attach 到该 target 对应的 next_hop。
- 若操作返回 transport 或 Mini9P 错误，则对应 route 可标记为 `OFFLINE`。
- 不在第一版中自动重连。

## 4. 去中心化路径与转发模型

### 4.1 责任划分

去中心化场景下，路径/路由判断由 `cluster_vfs` 或 router 层完成，`mini9p_client` 不参与全局路由。

```text
cluster_vfs/router:
  解析 /mcu3/dev/temp
  查 route table
  决定下一跳是 mcu1

mini9p_client:
  只负责向 client_to_mcu1 对应的 peer 发送 Mini9P 请求
```

### 4.2 会话方向与 fid 作用域

Mini9P 会话在文件访问语义上是单向的：

```text
client -> server: Tattach/Twalk/Topen/Tread/Twrite/Tstat/Tclunk
server -> client: Rattach/Rwalk/Ropen/Rread/Rwrite/Rstat/Rclunk/Rerror
```

底层物理链路是双向的，因为请求和响应要往返；但一个 Mini9P 会话只表示“一个 client 访问一个 server 的文件树”。如果两个节点要互相访问，需要两个相反方向的会话。

`fid` 不是文件的真实全局 ID，而是某个 Mini9P 会话内的临时文件句柄。不同会话可以使用相同的 `fid`。在去中心化端到端模式下，`fid` 的完整作用域应视为：

```text
src_node + dst_node + session_id + fid
```

目标节点的 Mini9P server 需要按会话维护 fid 表；中间 router 不应解释 fid，也不应维护 upstream/downstream fid 映射。

### 4.3 不采用 Mini9P proxy 转发

去中心化多上游场景下，不建议让中间节点作为 Mini9P proxy 维护 fid 映射。原因是多个上游节点都可能使用相同 fid，例如：

```text
A -> Router: fid=1
B -> Router: fid=1
C -> Router: fid=1
```

若 router 做 proxy，它必须维护：

```text
upstream_node + upstream_session + upstream_fid
  -> target_node + downstream_session + downstream_fid
```

这会引入断线清理、fid 回收、错误传播和并发状态问题，复杂度过高。

推荐方向是端到端 Mini9P + 轻量 routing envelope/header：

```text
routing envelope: src_node, dst_node, session_id, ttl
payload:          原始 Mini9P frame
```

中间 router 只看 `dst_node` 和 `ttl`，查本地 route table 后原样转发 payload；只有目标节点才解析 Mini9P frame 和维护 fid 表。ttl 是 Time To Live，在这里不是时间，而是“这个包最多还能被转发多少跳”。

当前 MVP 为了少改现有协议，可先采用全局路径转发；但长期去中心化方案应优先演进到 routing envelope/header。

### 4.4 直连路径

直连场景中，发送给 leaf server 的路径可以是目标节点本地路径：

```text
用户路径: /mcu1/dev/temp
目标节点: mcu1
下一跳:   mcu1
发送路径: /dev/temp
```

### 4.5 中继路径

中继场景中，下一跳不是最终目标。为了让中继节点知道最终目标，第一版设计预留两种方案：

#### 方案 A：全局路径转发

发给 router 节点时保留全局路径：

```text
用户路径: /mcu3/dev/temp
目标节点: mcu3
下一跳:   mcu1
发送路径: /mcu3/dev/temp
```

`mcu1` 的 `cluster_vfs/router` 收到后继续查表。如果 `mcu1` 直连 `mcu3`，再转成 `/dev/temp` 发给 `mcu3`。

优点：不需要额外外层包头。  
缺点：router 节点的 server 必须理解 `/mcuN/...` 全局路径。

#### 方案 B：外层 routing header

在 Mini9P 外层增加轻量路由头：

```text
target=mcu3
Mini9P payload path=/dev/temp
```

中间节点只看外层 `target` 转发，不解析 Mini9P payload。

优点：Mini9P 仍保持本地路径语义，router 更干净。  
缺点：需要额外设计 routing header 和转发层。

当前 MVP 优先实现方案 A 的静态一跳中继：发给 router 节点时保留全局路径，例如 `/mcu3/dev/temp`。router 节点收到后再用自己的路由表决定下一跳。方案 B 的外层 routing header 留作后续更干净的协议演进方向。

## 5. 对外 API 设计

### 5.1 初始化与路由管理

```c
int cluster_vfs_init(void);

int cluster_vfs_add_direct(const char *target,
                           struct m9p_client *client);

int cluster_vfs_add_route(const char *target,
                          const char *next_hop,
                          struct m9p_client *client);

int cluster_vfs_remove_route(const char *target);

int cluster_vfs_attach(const char *target);
int cluster_vfs_detach(const char *target);
```

兼容中心式命名时，可以保留 `mount/unmount` 作为直连路由的别名：

```c
int cluster_vfs_mount(const char *name, struct m9p_client *client);
int cluster_vfs_unmount(const char *name);
```

等价关系：

```text
cluster_vfs_mount("mcu1", client)
  == cluster_vfs_add_direct("mcu1", client)

cluster_vfs_add_direct("mcu1", client)
  == cluster_vfs_add_route("mcu1", "mcu1", client)
```

说明：

- `target` 使用 `"mcu1"` 这种不带 `/` 的全局节点名。
- `next_hop` 是下一跳节点名。直连时 `target == next_hop`。
- `client` 由上层初始化，可接 mock transport、UART transport 或其他 transport，表示通往 `next_hop` 的连接。
- 第一版不负责创建 `m9p_client`，只保存指针。
- `cluster_vfs_attach(target)` 实际 attach 的是该 target 的 `next_hop` client。
- 是否直连不单独保存字段，由 `strcmp(target, next_hop) == 0` 推导。

### 5.2 类 POSIX 文件接口

```c
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

### 5.3 路径级快捷接口

为了方便 Shell/Lua/Web，建议同时提供路径级快捷接口：

```c
int cluster_vfs_read_path(const char *path,
                          uint8_t *buf,
                          uint16_t *in_out_len);

int cluster_vfs_write_path(const char *path,
                           const uint8_t *data,
                           uint16_t len,
                           uint16_t *out_written);
```

这些接口内部执行：

```text
open -> read/write -> close
```

### 5.4 目录读取

Mini9P 规范中目录读取通过 `open(dir, OREAD) + read(dir)` 完成。初代可以先提供：

```c
int cluster_vfs_list(const char *path,
                     struct m9p_dirent *entries,
                     size_t max_entries,
                     size_t *out_count);
```

第一版可分两种情况：

- `cluster_vfs_list("/")` 返回 Master 侧虚拟挂载点目录，如 `mcu1`、`mcu2`。
- `cluster_vfs_list("/mcu1/dev")` 转发到从机目录读取。

如果从机目录读取暂时未稳定，可先只实现根目录列出挂载点。

## 6. 路径路由流程

以 `/mcu1/dev/temp` 为例：

```text
1. 检查路径必须以 '/' 开头。
2. 取第一段路径，得到 target = "mcu1"。
3. 在 route table 中查找 target。
4. 得到 next_hop 和 client。
5. 如果 `target == next_hop`，将剩余路径转换为 remote_path = "/dev/temp"。
6. 如果 `target != next_hop`，MVP 使用全局路径转发：发送原始路径 "/mcu1/dev/temp" 给下一跳 router。
7. 调用 m9p_client_open_path(client, send_path, mode, ...).
8. read/write/stat/close 继续转发到该 route 对应的 client。
```

特殊路径规则：

```text
/             Master 虚拟根目录
/mcu1         mcu1 从机根目录 "/"
/mcu1/        mcu1 从机根目录 "/"
/mcu1/a/b     target == next_hop 时发送本地路径 "/a/b"
```

错误路径：

```text
mcu1/a        非绝对路径，返回 EINVAL
/mcu9/a       无路由，返回 ENOENT
/mcu1/a       route 离线，返回 EIO 或 EAGAIN
超长路径      返回 EMSIZE
```

## 7. MVP 范围

### 7.1 支持内容

初代版本支持：

- 最多 8 条静态路由。
- 直连路由：`target == next_hop`。
- 静态一跳中继路由：`target != next_hop`，通过下一跳 router 转发全局路径。
- `open/read/write/stat/close`。
- 路径级 `read_path/write_path`。
- `/` 虚拟根目录列出已知 target。
- `/mcuN/...` 到从机本地路径的转换。
- 单线程或低并发调用。
- mock transport 下的逻辑验证。

### 7.2 暂不支持内容

初代版本不支持：

- 动态节点发现。
- 超过一跳的多跳转发。
- 外层 routing header。
- 动态路由协议和路径选择算法。
- 多 Tag 并发调度。
- 目录项缓存。
- 权限系统。
- 路径中的复杂规范化，如多重 `//`、`.`、`..`。
- 跨节点 rename/copy。
- 自动重连和心跳。
- 完整 POSIX fd 语义。
- 并发锁。

## 8. 错误码策略

建议复用 Mini9P 负错误码风格：

```text
-M9P_ERR_EINVAL   参数或路径非法
-M9P_ERR_ENOENT   节点或路径不存在
-M9P_ERR_EIO      transport 或远端 I/O 失败
-M9P_ERR_EFID     本地 fd 无效
-M9P_ERR_EMSIZE   路径或缓冲区超限
-M9P_ERR_ENOTSUP  当前操作暂不支持
-M9P_ERR_EAGAIN   节点暂时不可用
```

`cluster_vfs` 不另造一套复杂错误码，避免 Shell/Lua/Web 做多套错误转换。

## 9. 实现步骤与验证方式

### Step 1: 建立模块骨架

文件：

```text
pwos-master-esp32p4/vfs_bridge/cluster_vfs.h
pwos-master-esp32p4/vfs_bridge/cluster_vfs.c
```

内容：

- include guard。
- 公开 API。
- 内部静态 route table 和 open 表。

验证：

- 普通 C 编译通过。
- 不引入 ESP-IDF 头文件。

### Step 2: 实现路由表

实现：

- `cluster_vfs_init`
- `cluster_vfs_add_direct`
- `cluster_vfs_add_route`
- `cluster_vfs_remove_route`
- `cluster_vfs_mount` 和 `cluster_vfs_unmount` 兼容别名
- 内部 `find_route`

验证：

- 添加直连路由 `mcu1` 成功。
- `cluster_vfs_mount("mcu1", client)` 等价于直连路由。
- 重复添加同一 target 返回错误。
- 超过 `CLUSTER_VFS_MAX_ROUTES` 返回错误。
- remove route 后查找失败。
- 添加间接路由 `target=mcu3,next_hop=mcu1` 成功。
- 访问 `/mcu3/...` 时会选择 `client_to_mcu1`，发送给下一跳的路径保持全局路径 `/mcu3/...`。

### Step 3: 实现路径解析

实现内部函数：

```c
static int resolve_path(const char *path,
                        struct cluster_vfs_route **out_route,
                        char *remote_path,
                        size_t remote_cap);
```

验证样例：

```text
/mcu1/sys/health -> route(mcu1), /sys/health
/mcu1            -> route(mcu1), /
/mcu1/           -> route(mcu1), /
/                 -> virtual root
mcu1/sys         -> EINVAL
/mcu9/sys        -> ENOENT
```

### Step 4: 实现 open/close

实现：

- `cluster_vfs_open`
- `cluster_vfs_close`
- open 表分配与释放

流程：

```text
resolve path
-> m9p_client_open_path
-> 保存 remote_fid
-> 返回 local_fd
```

验证：

- 使用 mock client 时能观察到 `m9p_client_open_path` 被调用。
- close 后调用 `m9p_client_clunk`。
- close 无效 fd 返回 `EFID`。
- 若 `target != next_hop`，MVP 不剥离 `/mcuN` 前缀，而是把全局路径发给下一跳 router。

### Step 5: 实现 read/write/stat

实现：

- `cluster_vfs_read`
- `cluster_vfs_write`
- `cluster_vfs_stat`
- `cluster_vfs_read_path`
- `cluster_vfs_write_path`

验证：

- read 路径走 `open -> read -> close`。
- write 路径走 `open -> write -> close`。
- stat 直接 resolve 后调用远端 stat 流程。
- 操作失败时释放已分配 fd。

### Step 6: 实现 list

第一阶段：

- `cluster_vfs_list("/")` 返回挂载节点。

第二阶段：

- `cluster_vfs_list("/mcu1/dev")` 转发到 Mini9P 目录读取。

验证：

- Shell `ls /` 能列出 `mcu1`。
- 后续从机目录读取稳定后，`ls /mcu1/dev` 能列出远端目录项。

### Step 7: 接入 Shell

初代建议改造：

```text
cat <path>       -> cluster_vfs_read_path
write <path> x   -> cluster_vfs_write_path
stat <path>      -> cluster_vfs_stat
ls <path>        -> cluster_vfs_list
```

`m9p_attach`、`m9p_walk` 可继续作为低层调试命令保留，不强制走 VFS。

验证：

```text
cat /mcu1/sys/health
write /mcu1/dev/led 1
ls /
```

在真实 transport 未完成前，可以使用 mock transport 验证调用链和错误返回。

## 10. 与上下游模块的关系

### 10.1 上游

Shell/Lua/Web 只应看到路径级接口：

```text
read("/mcu1/sys/health")
write("/mcu1/dev/led", "1")
stat("/mcu1/sys/health")
list("/mcu1/dev")
```

上游不应直接处理：

```text
fid
qid
Twalk
Topen
Tclunk
transport
```

### 10.2 下游

`cluster_vfs` 下游只调用 Mini9P client：

```text
m9p_client_open_path
m9p_client_read
m9p_client_write
m9p_client_stat
m9p_client_clunk
```

下游 client 的 transport 可以是：

```text
mock_transport
UART transport
SPI transport
WiFi transport
```

`cluster_vfs` 不需要知道 transport 的具体类型。

### 10.3 去中心化节点关系

完全去中心化时，具备转发能力的节点需要同时拥有：

```text
mini9P server       接收上游请求
cluster_vfs/router  判断目标是否为自己，或查表选择下一跳
mini9P client       向下一跳发起请求
local file tree     处理目标为自己的 /sys /dev /compute
```

leaf STM32 节点可以只保留：

```text
mini9P server + local file tree
```

不强制所有资源受限节点都承担转发。

## 11. 后续扩展方向

MVP 跑通后可考虑：

- 节点心跳与在线状态文件。
- 自动 attach/re-attach。
- route table 动态更新。
- routing header 设计，用于替代当前 MVP 的全局路径转发。
- 超过一跳的多跳转发。
- 挂载点/路由目录缓存。
- 多从机并发访问锁。
- Lua 绑定 `read/write/stat/list`。
- Web 文件管理器直接调用 VFS。
- 节点发现和动态挂载。
- 简单权限和只读标志。
- 长任务文件语义封装，例如 `/mcu1/compute/jacobi/status`。

## 12. 初代完成标准

初代可认为完成，当满足：

```text
1. 可以添加至少一条直连路由 /mcu1 -> client_to_mcu1。
2. 可以解析 /mcu1/... 为从机本地路径。
3. 可以通过 cluster_vfs_read_path/write_path 调到 m9p_client。
4. 可以维护 local fd -> remote fid 映射。
5. close 会释放远端 fid。
6. Shell 可以通过 cluster_vfs 访问 /mcu1/... 路径。
7. 可以添加一条静态一跳中继路由 /mcu3 -> next_hop=mcu1。
8. 访问 /mcu3/... 时能选择 client_to_mcu1，并保留全局路径发给下一跳。
9. route table 使用 target/next_hop/client 表达去中心化路由，不保存 direct/hop_count 这类可推导字段。
10. 整个模块不依赖 ESP-IDF，后续可接 mock 或真实 UART transport。
```
