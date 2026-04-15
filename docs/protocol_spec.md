# mini9P 协议规范（v1）

## 1. 设计目标

mini9P 是面向本项目 MCU 主从集群的一套极简远程文件协议。它参考 9P 的核心思想，但只保留当前阶段真正需要的能力，用于把从节点上的虚拟文件树挂载到主控 VFS 中。

v1 的目标只有四个：

- 支撑 `ls`、`cat`、`echo`、`stat` 这一组最小 Shell 交互。
- 适配 UART 优先的低带宽、低内存场景，并可平滑迁移到 SPI/WiFi。
- 保证 Slave 端实现不依赖动态内存，全部使用静态缓冲区和固定表项。
- 保持协议语义接近 9P，方便后续继续扩展而不是推倒重来。

## 2. v1 非目标

下面这些能力明确不进入 v1：

- 不实现完整 9P2000。
- 不实现 `auth`、`flush`、`create`、`remove`、`rename`、`wstat`。
- 不做分布式锁和缓存一致性协议。
- 不支持让一次 RPC 长时间阻塞。

最后这一点非常关键。对于 Jacobi、矩阵乘法这类耗时任务，mini9P 不允许某个 `Twrite` 或 `Tread` 在链路上挂很久。正确做法是把任务设计成文件语义：

- 向命令文件写参数，立即返回。
- 通过状态文件轮询任务状态。
- 从结果文件读取最终输出。

这样可以避免在没有 `flush` 的前提下把链路状态做复杂。

## 3. 协议作用域

### 3.1 连接模型

- 一条 Master 到 Slave 的链路，对应一个 mini9P 会话。
- 会话建立后，Master 通过 `fid` 管理远端文件句柄。
- 断链后，Slave 必须丢弃该会话的全部 `fid` 状态。

### 3.2 路径作用域

mini9P 只处理节点本地路径，不处理集群前缀。

例如主控 VFS 中的路径是：

```text
/mcu1/temperature
/mcu1/motor/speed
/mcu2/compute/jacobi/result
```

但发到对应 Slave 的 mini9P `Twalk` 中，路径应当是：

```text
/temperature
/motor/speed
/compute/jacobi/result
```

也就是说，`/mcuN` 这一层命名空间聚合由 Master 的 `cluster_vfs` 完成，mini9P 只看节点本地文件树。

### 3.3 推荐文件树约定

协议本身不强制目录结构，但建议 Slave 统一暴露以下根目录：

- `/sys`：节点信息、版本、健康状态、心跳、统计信息。
- `/dev`：传感器、GPIO、电机等设备文件。
- `/compute`：计算任务入口、状态和结果。

这样主控做聚合时更容易形成统一命名空间。

## 4. 线缆上的帧格式

mini9P v1 采用固定包头 + 变长载荷 + CRC 的帧结构，直接适用于 UART/SPI；放到 TCP/WiFi 上时也保持同样格式，避免协议再分层。

### 4.1 基本帧结构

```text
| Magic(2) | FrameLen(2) | Version(1) | Type(1) | Tag(2) | Payload(N) | CRC16(2) |
```

字段说明：

| 字段 | 大小 | 说明 |
|------|------|------|
| `Magic` | 2B | 固定为 ASCII `9P`，用于流中重同步 |
| `FrameLen` | 2B | 从 `Version` 到 `Payload` 末尾的总字节数 |
| `Version` | 1B | 协议版本，v1 固定为 `0x01` |
| `Type` | 1B | 消息类型 |
| `Tag` | 2B | 请求/响应配对标识，小端序 |
| `Payload` | N | 具体消息负载 |
| `CRC16` | 2B | 对 `Version..Payload` 计算 CRC-16/CCITT-FALSE |

约定：

- 所有整数均为小端序。
- 所有字符串均为长度前缀，不以 `NUL` 结尾。
- 所有结构体按 packed 布局解释，不允许隐式对齐填充。

### 4.2 尺寸约束

v1 采用 `msize` 协商最大消息大小，单位就是 `FrameLen`。

- 最小实现要求：`msize >= 128`
- 推荐值：`msize = 256`
- 上限建议：`msize <= 1024`

这样在 UART 阶段不会把单帧做得太大，也能给路径、目录项和小块数据留下足够空间。

### 4.3 CRC 和超时

- CRC 校验失败时，接收端直接丢弃该帧，不返回错误响应。
- 发送端通过超时重传处理丢帧。
- 重传时必须保持同一个 `Tag` 和完全相同的请求内容。

## 5. 核心对象

### 5.1 Tag

`Tag` 用来标识一个 RPC 请求及其响应。

v1 约束：

- 同一会话内，一个 `Tag` 在收到最终响应前不得复用。
- Master 可根据 `Rattach` 返回的 `max_inflight` 决定是否允许多请求并发。
- 最小实现允许 `max_inflight = 1`。

为了让重传安全，Slave 应缓存最近一次完成的响应：

- 若收到相同 `Tag` 且请求内容一致，则直接重发缓存响应。
- 不重复执行有副作用的操作。

这也是 v1 即使并发很低，依然保留 `Tag` 字段的核心原因。

### 5.2 Fid

`fid` 是远端文件对象句柄，用于表示一个已经 attach/walk 到位的节点。

v1 约束：

- `fid` 为 16 位无符号整数。
- 一个活跃 `fid` 在 `clunk` 前一直有效。
- 已 `open` 的 `fid` 不允许再次 `walk`。
- `clunk` 后该 `fid` 立即失效，可被重新分配。

### 5.3 Qid

`qid` 是节点内稳定对象标识，用于表达“这是哪个文件/目录”。v1 使用 8 字节定长结构：

```c
struct m9p_qid {
    uint8_t type;
    uint8_t reserved;
    uint16_t version;
    uint32_t object_id;
};
```

其中 `type` 采用位标志：

| 位 | 含义 |
|----|------|
| `0x80` | 目录 |
| `0x08` | 虚拟节点 |
| `0x04` | 设备节点 |
| `0x02` | 计算节点 |
| `0x01` | 只读 |

`object_id` 只需在同一 Slave 内稳定，不要求全局唯一。

## 6. 消息类型

v1 使用请求/响应成对编码，请求码在低半区，响应码统一把最高位设为 1。

| 类型 | 数值 |
|------|------|
| `Tattach` | `0x01` |
| `Rattach` | `0x81` |
| `Twalk` | `0x02` |
| `Rwalk` | `0x82` |
| `Topen` | `0x03` |
| `Ropen` | `0x83` |
| `Tread` | `0x04` |
| `Rread` | `0x84` |
| `Twrite` | `0x05` |
| `Rwrite` | `0x85` |
| `Tstat` | `0x06` |
| `Rstat` | `0x86` |
| `Tclunk` | `0x07` |
| `Rclunk` | `0x87` |
| `Rerror` | `0xFF` |

## 7. 负载格式

### 7.1 Tattach / Rattach

`Tattach` 用于建立会话并把一个 `fid` 绑定到远端根目录。为了保持协议精简，v1 不单独设计 `Tversion`，版本协商直接放在 `attach` 中完成。

`Tattach` 负载：

```c
struct m9p_tattach {
    uint16_t fid;
    uint16_t requested_msize;
    uint8_t requested_inflight;
    uint8_t attach_flags;
};
```

`attach_flags`：

- `0x01`：内核态/系统会话
- `0x02`：只读会话

`Rattach` 负载：

```c
struct m9p_rattach {
    uint16_t negotiated_msize;
    uint8_t max_fids;
    uint8_t max_inflight;
    uint32_t feature_bits;
    struct m9p_qid root_qid;
};
```

`feature_bits`：

- `0x00000001`：支持目录读取
- `0x00000002`：支持多 `Tag` 并发
- `0x00000004`：支持重复请求响应缓存
- `0x00000008`：识别 `attach_flags`

最小实现要求：

- 必须返回根目录 `qid`
- 必须至少支持 `max_fids >= 8`
- 可返回 `max_inflight = 1`

### 7.2 Twalk / Rwalk

`walk` 用于从一个现有 `fid` 导出另一个目标 `fid`，并完成路径解析。

`Twalk` 负载：

```c
struct m9p_twalk {
    uint16_t fid;
    uint16_t newfid;
    uint8_t path_len;
    uint8_t path[path_len];
};
```

语义约束：

- `path` 允许绝对路径和相对路径。
- `path_len == 0` 表示克隆当前 `fid` 到 `newfid`。
- 除空路径克隆外，`newfid` 必须与 `fid` 不同。
- 路径解析要么完全成功，要么失败；v1 不支持部分 walk 成功。
- 建议支持 `.` 和 `..`，其中根目录的 `..` 仍指向根目录。

`Rwalk` 负载：

```c
struct m9p_rwalk {
    struct m9p_qid qid;
};
```

### 7.3 Topen / Ropen

`open` 用于把一个已 walk 到位的 `fid` 置为可读/可写状态。

`Topen` 负载：

```c
struct m9p_topen {
    uint16_t fid;
    uint8_t mode;
};
```

`mode` 约定：

- `0x00`：只读
- `0x01`：只写
- `0x02`：读写
- `0x10`：截断写

说明：

- 目录只允许以只读方式打开。
- 若目标节点不支持对应访问模式，则返回 `Rerror(EPERM)`。

`Ropen` 负载：

```c
struct m9p_ropen {
    struct m9p_qid qid;
    uint16_t iounit;
};
```

`iounit` 表示该 `fid` 单次 `read`/`write` 推荐的最大数据量。

### 7.4 Tread / Rread

`Tread` 负载：

```c
struct m9p_tread {
    uint16_t fid;
    uint32_t offset;
    uint16_t count;
};
```

`Rread` 负载：

```c
struct m9p_rread {
    uint16_t count;
    uint8_t data[count];
};
```

语义约束：

- 对普通文件，`offset` 表示字节偏移。
- 对目录，`offset` 表示目录项索引，而不是字节偏移。
- 返回 `count == 0` 表示 EOF 或目录遍历结束。
- 返回数据长度不得超过协商后的 `msize` 和当前 `fid` 的 `iounit`。

之所以把目录 `offset` 解释为“目录项索引”，是为了让 Slave 不需要在内存里先拼出整块目录数据再切片，便于静态实现。

### 7.5 Twrite / Rwrite

`Twrite` 负载：

```c
struct m9p_twrite {
    uint16_t fid;
    uint32_t offset;
    uint16_t count;
    uint8_t data[count];
};
```

`Rwrite` 负载：

```c
struct m9p_rwrite {
    uint16_t count;
};
```

语义约束：

- 对标量控制文件，通常只保证 `offset == 0`。
- 对需要流式写入的普通文件，可支持非零 `offset`。
- 对命令文件，`write` 应尽快返回，不得在链路上长时间阻塞。
- 若 `count` 大于 `iounit`，返回 `Rerror(EMSIZE)`。

### 7.6 Tstat / Rstat

`Tstat` 负载：

```c
struct m9p_tstat {
    uint16_t fid;
};
```

`Rstat` 负载：

```c
struct m9p_rstat {
    struct m9p_qid qid;
    uint8_t perm;
    uint8_t flags;
    uint32_t size;
    uint32_t mtime;
    uint8_t name_len;
    uint8_t name[name_len];
};
```

`perm` 位定义：

- `0x01`：可读
- `0x02`：可写
- `0x04`：可执行/可触发

`flags` 位定义：

- `0x01`：目录
- `0x02`：虚拟节点
- `0x04`：设备节点
- `0x08`：计算节点

补充语义：

- 对普通文件，`size` 表示字节数。
- 对目录，`size` 表示子项个数。
- `mtime` 使用节点本地单调时间戳或 RTC 秒值即可，不要求 Unix epoch 严格一致。

### 7.7 Tclunk / Rclunk

`clunk` 用于释放 `fid`。

`Tclunk` 负载：

```c
struct m9p_tclunk {
    uint16_t fid;
};
```

`Rclunk` 无负载。

语义约束：

- `clunk` 必须幂等。
- 若 `fid` 已不存在，可返回成功，也可返回 `EFID`；推荐直接返回成功，简化回收逻辑。

### 7.8 Rerror

任意请求失败时，统一返回 `Rerror`。

```c
struct m9p_rerror {
    uint16_t errcode;
    uint8_t msg_len;
    uint8_t msg[msg_len];
};
```

`msg` 建议使用 ASCII 短字符串，仅用于调试日志，不作为业务判断依据。

## 8. 目录读取编码

目录通过 `open(dir, OREAD) + read(dir)` 实现，不单独定义 `readdir`。

`Rread.data` 中的目录项使用如下变长结构顺序拼接：

```c
struct m9p_dirent {
    struct m9p_qid qid;
    uint8_t perm;
    uint8_t flags;
    uint8_t name_len;
    uint8_t name[name_len];
};
```

约束：

- 一个目录项不能被拆成两半返回。
- Slave 在 `count` 限制内尽可能多地返回完整目录项。
- 若下一个目录项放不下，则本次 `Rread` 结束。
- 客户端用“已消费目录项数”作为下一次目录 `offset`。

这样 `ls` 的流程可以保持很简单：

1. `attach`
2. `walk` 到目录
3. `open(OREAD)`
4. `read(offset = 0)`
5. 解析若干 `dirent`
6. `read(offset = 已返回目录项数)` 直到 EOF

## 9. 错误码

v1 统一使用协议内错误码，不直接传输 POSIX errno。

| 错误码 | 名称 | 含义 |
|--------|------|------|
| `0x0001` | `EINVAL` | 字段非法、模式非法、路径格式非法 |
| `0x0002` | `ENOENT` | 路径不存在或目标不存在 |
| `0x0003` | `EPERM` | 权限不足 |
| `0x0004` | `EBUSY` | 资源忙、`fid` 正在使用、文件已打开 |
| `0x0005` | `ENOTDIR` | 期望目录但目标不是目录 |
| `0x0006` | `EISDIR` | 期望普通文件但目标是目录 |
| `0x0007` | `EOFFS` | 不支持该 `offset` |
| `0x0008` | `EMSIZE` | 消息过大或超过 `iounit` |
| `0x0009` | `EIO` | 驱动或底层 I/O 错误 |
| `0x000A` | `ENOTSUP` | 操作未实现 |
| `0x000B` | `ETAG` | `tag` 冲突或重放不合法 |
| `0x000C` | `EFID` | `fid` 非法、重复或不存在 |
| `0x000D` | `EAGAIN` | 资源暂时不可用，建议稍后重试 |

## 10. 状态机约束

客户端与服务端都必须遵守下面这组最小状态机：

```text
Detached
  -> attach
Attached(root fid live)
  -> walk(new fid live)
Walked
  -> open
Opened
  -> read/write/stat
  -> clunk
Clunked/Detached
```

补充规则：

- `stat` 可作用在未 `open` 的活跃 `fid` 上。
- `read/write` 必须作用在已 `open` 的 `fid` 上。
- `walk` 不得作用在已 `open` 的 `fid` 上。
- 断链等价于对该会话全部 `fid` 执行隐式 `clunk`。

## 11. 推荐超时与重传策略

v1 采用“短超时 + 同 Tag 重传 + 会话重建”的简单策略：

- UART 阶段建议单次请求超时 10 ms 到 50 ms。
- 建议最多重传 3 次。
- 超过重传上限后，Master 主动丢弃当前会话并重新 `attach`。

为了让重传不产生副作用，Slave 建议维护：

- 一个活跃请求表，大小为 `max_inflight`
- 一个最近响应缓存，至少缓存最近 1 个完成响应

## 12. 典型交互

### 12.1 读取温度

主控执行：

```text
cat /mcu1/temperature
```

VFS 到 mini9P 的序列：

```text
Tattach(fid=1)
Twalk(fid=1, newfid=2, path="/temperature")
Topen(fid=2, mode=OREAD)
Tread(fid=2, offset=0, count=32)
Tclunk(fid=2)
```

### 12.2 读取目录

主控执行：

```text
ls /mcu1/dev
```

VFS 到 mini9P 的序列：

```text
Tattach(fid=1)
Twalk(fid=1, newfid=2, path="/dev")
Topen(fid=2, mode=OREAD)
Tread(fid=2, offset=0, count=128)
Tread(fid=2, offset=已返回目录项数, count=128)
Tclunk(fid=2)
```

### 12.3 下发计算任务

主控执行：

```text
echo "n=32,iters=10" > /mcu2/compute/jacobi/cmd
cat /mcu2/compute/jacobi/status
cat /mcu2/compute/jacobi/result
```

推荐文件语义：

- `/compute/jacobi/cmd`：写入任务参数，立即返回
- `/compute/jacobi/status`：读取当前状态，如 `idle/running/done/error`
- `/compute/jacobi/result`：任务完成后读取结果

这样可以避免把“长计算”塞进单个 RPC 的响应时间里。

## 13. 与当前仓库的落地对应

按照现有目录规划，建议分工如下：

- `master/main/rpc_client/`：负责编码/发送/重传 mini9P 请求并解析响应。
- `master/main/vfs_bridge/`：负责把 `/mcuN/...` 转为节点本地路径，并维护 `fid` 生命周期。
- `master/components/mini9p/`：放协议公共头文件，如消息类型、错误码、结构体定义。
- `slave/rpc_server/`：负责编码解析、`fid` 表、`tag` 缓存、分发到文件树回调。
- `slave/fs/`：提供目录树、`read/write/stat/open` 回调接口。

## 14. v2 预留扩展

如果 v1 跑通，后续建议按下面顺序扩展，而不是一次性把协议做复杂：

1. 多 `Tag` 并发与乱序响应
2. `flush`/取消语义
3. `create`/`remove`/`rename`
4. 轻量鉴权
5. 节点发现与广播
6. WiFi 下更大的 `msize`

v1 的关键不是“像 9P 一样完整”，而是先把主控 VFS 到 Slave 虚拟文件树这条链路稳定打通。