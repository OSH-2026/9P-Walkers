# 9P-Walkers 协议规范（v1）

本项目的通信协议分为两层：

1. **Mesh Envelope v1**：负责节点编址、多跳转发、控制面消息（发现、分配、路由、拓扑）。
2. **mini9P v1**：负责文件访问语义（attach/walk/open/read/write/stat/clunk），作为 `MESH_TYPE_MINI9P` 的 payload 被 mesh envelope 承载。

> 线缆上实际传输的是 **mesh envelope 帧**，mini9P 帧不再直接出现。旧版文档中“mini9P 直接跑在 UART/SPI/WiFi 上”的描述已过时。

---

## 1. Mesh Envelope 层

Mesh Envelope 是项目当前的底层网络协议，定义在 `pwos-shared/mesh/envelope/mesh_protocal.h`。

详细规范见：

- `pwos-shared/mesh/envelope/mesh_protocol_spec.md`
- `docs/mesh_envelope_spec.md`（本文档的镜像副本，便于顶层查阅）

### 1.1 帧格式

```text
| Magic(2) | FrameLen(2) | Version(1) | Type(1) | Src(1) | Dst(1) |
| Seq(2)   | Hop(1)      | Flags(1)   | Payload(N)             | CRC16(2) |
```

- `Magic = "MH"`。
- `FrameLen = 8 + payload_len`；整帧长度 = `FrameLen + 6`。
- `Version = 0x01`；所有整数小端序。
- `Src/Dst`：短地址，`0x00` 为主机，`0xFF` 为未分配。
- `Hop`：剩余跳数，转发时递减；到 0 丢帧。
- `Flags`：`NEEDS_ACK(0x01)` / `IS_RETRY(0x02)` / `CONTROL(0x04)`。
- CRC-16/CCITT-FALSE 覆盖 `Version..Payload`。

### 1.2 消息类型

| 类型 | 值 | 平面 | 用途 |
|---|---:|---|---|
| `MESH_TYPE_MINI9P` | 0x01 | 数据面 | 承载完整 mini9P 帧 |
| `MESH_TYPE_REGISTER` | 0x10 | 控制面 | 新节点 bootstrap 注册 |
| `MESH_TYPE_ASSIGN` | 0x11 | 控制面 | 主机分配地址与节点名 |
| `MESH_TYPE_PING` | 0x12 | 控制面 | 探活请求 |
| `MESH_TYPE_PONG` | 0x13 | 控制面 | 探活响应 |
| `MESH_TYPE_TIME_SYNC` | 0x14 | 控制面 | 四时间戳同步 |
| `MESH_TYPE_ROUTE_UPDATE` | 0x15 | 控制面 | 路由项更新 |
| `MESH_TYPE_LINK_STATE` | 0x16 | 控制面 | 邻居链路状态上报 |
| `MESH_TYPE_NEIGHBOR_PROBE_REQUEST` | 0x17 | 控制面 | 单链路邻居探测请求 |
| `MESH_TYPE_NEIGHBOR_PROBE_RESPONSE` | 0x18 | 控制面 | 单链路邻居探测响应 |
| `MESH_TYPE_ERROR` | 0x7F | 控制面 | 错误上报 |

### 1.3 转发规则

中继节点：

1. 校验结构、Magic、CRC。
2. 若 `Dst == local_addr`，交本地处理。
3. 若 `Hop == 0`，丢弃。
4. 否则 `Hop--`，查路由表转发。
5. **不解析 mini9P payload**。

---

## 2. mini9P 层

mini9P 是面向 MCU 集群的极简远程文件协议，参考 9P 的核心思想但只保留当前阶段需要的能力。

完整帧格式与消息类型详解见：

- `pwos-master-esp32p4/README.md#mini9P-协议帧格式`
- `pwos-shared/mini9p/README.md`

### 2.1 设计目标

- 支撑 `ls`、`cat`、`echo`、`stat` 等最小 Shell 交互。
- 适配低带宽、低内存场景。
- 从机实现不依赖动态内存，使用静态缓冲区和固定表项。
- 协议语义接近 9P，便于后续扩展。

### 2.2 v1 非目标

- 不实现完整 9P2000。
- 不实现 `auth`、`flush`、`create`、`remove`、`rename`、`wstat`。
- 不做分布式锁和缓存一致性协议。
- 不支持让一次 RPC 长时间阻塞。

耗时任务（如 Jacobi、矩阵乘法）应设计为文件语义：

- 向命令文件写参数，立即返回。
- 通过状态文件轮询任务状态。
- 从结果文件读取最终输出。

### 2.3 基本帧结构

mini9P v1 帧作为 mesh `MESH_TYPE_MINI9P` 的 payload 出现：

```text
| Magic(2) | FrameLen(2) | Version(1) | Type(1) | Tag(2) | Payload(N) | CRC16(2) |
```

- `Magic = 0x39 0x50`（即 ASCII `'9P'`）。
- `FrameLen = 4 + payload_len`；总长度 = `10 + payload_len`。
- `Version = 0x01`。
- `Type`：最高位为 1 表示响应（R*），为 0 表示请求（T*）。
- `Tag`：事务标签，请求与对应响应 tag 相同。
- CRC-16/CCITT-FALSE 覆盖 `Version..Payload`。

### 2.4 消息类型

| 类型 | 值 | 方向 | 说明 |
|---|---:|---|---|
| Tattach | 0x01 | C→S | 建立会话 |
| Rattach | 0x81 | S→C | 会话确认 |
| Twalk | 0x02 | C→S | 路径遍历 |
| Rwalk | 0x82 | S→C | 返回 QID |
| Topen | 0x03 | C→S | 打开对象 |
| Ropen | 0x83 | S→C | 打开确认 |
| Tread | 0x04 | C→S | 读取数据 |
| Rread | 0x84 | S→C | 返回数据 |
| Twrite | 0x05 | C→S | 写入数据 |
| Rwrite | 0x85 | S→C | 写入确认 |
| Tstat | 0x06 | C→S | 查询元数据 |
| Rstat | 0x86 | S→C | 返回元数据 |
| Tclunk | 0x07 | C→S | 释放 fid |
| Rclunk | 0x87 | S→C | 释放确认 |
| Rerror | 0xFF | S→C | 通用错误响应 |

### 2.5 典型访问流程

1. `Tattach(fid=0)`：建立会话，绑定根目录。
2. `Twalk(fid=0, newfid=1, path="dev/temp")`：解析路径。
3. `Topen(fid=1)`：打开目标对象。
4. `Tread` / `Twrite` / `Tstat`：数据与元数据操作。
5. `Tclunk(fid=1)`：释放 fid。

### 2.6 路径作用域

mini9P 只处理节点本地路径，不处理集群前缀。

主控 VFS 中的全局路径：

```text
/mcu1/temperature
/mcu1/motor/speed
/mcu2/compute/jacobi/result
```

发到对应 Slave 的 mini9P `Twalk` 中应为：

```text
/temperature
/motor/speed
/compute/jacobi/result
```

`/mcuN` 这一层命名空间聚合由主控 `cluster_vfs` 完成。

### 2.7 推荐文件树约定

协议不强制目录结构，但建议 Slave 统一暴露：

- `/sys`：节点信息、版本、健康状态、心跳、统计信息。
- `/dev`：传感器、GPIO、电机等设备文件。
- `/compute`：计算任务入口、状态和结果。

---

## 3. 两层协议的关系

```text
应用层：cat /mcu1/sys/health
    │
    ▼
cluster_vfs: /mcu1/sys/health → target=mcu1, local_path=/sys/health
    │
    ▼
mini9p_client: 构造 Tread 帧
    │
    ▼
mesh_host_runtime: 把 mini9P 帧封装成 MESH_TYPE_MINI9P
    │ src=host_addr, dst=mcu1_addr
    ▼
mesh_processer: route_lookup → 转发
    │
    ▼
mesh_uart_transport / mesh_wifi_link: 字节流传输
    │
    ▼
从机 mesh_node_runtime: 解出 mini9P Tread
    │
    ▼
mini9p_server → local_vfs backend
    │
    ▼
构造 Rread，反向封装回 mesh MINI9P，返回主机
```

---

## 4. 相关文档

- `docs/architecture.md`：系统架构与目录结构
- `docs/mesh_envelope_spec.md`：mesh envelope 详细规范
- `pwos-shared/mesh/envelope/mesh_protocol_spec.md`：同上
- `pwos-shared/mesh/cluster/mesh_cluster_spec.md`：cluster 拓扑与路由
- `pwos-shared/mesh/processer/mesh_processer_spec.md`：processor 分流规则
- `pwos-shared/mini9p/README.md`：mini9P client/server/service 说明
- `pwos-master-esp32p4/README.md#mini9P-协议帧格式`：完整 mini9P 帧实例
