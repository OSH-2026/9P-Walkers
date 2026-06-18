# 9P-Walkers 重构计划书

> 本文档是 `refactor` 分支的唯一重构基线。它同时是**架构蓝图**和**执行计划**：
> 既约束模块边界、所有权和协议演进，也规定每一步做什么、测什么、什么算通过、失败怎么办。
>
> 阅读顺序建议：第 1 章理解为什么重写 → 第 2 章理解终态 → 第 3-7 章理解架构 →
> 第 8 章开始按里程碑动手。已经熟悉架构的人可以直接跳到第 8 章。

---

## 目录

1. [为什么要重写：根因诊断](#1-为什么要重写根因诊断)
2. [终态愿景](#2-终态愿景)
3. [架构总览：双平面模型](#3-架构总览双平面模型)
4. [分层职责与所有权](#4-分层职责与所有权)
5. [协议栈设计](#5-协议栈设计)
6. [从机架构（STM32F407 + FreeRTOS）](#6-从机架构stm32f407--freertos)
7. [主机架构（ESP32-P4 + 主机间平面）](#7-主机架构esp32-p4--主机间平面)
8. [里程碑与执行计划](#8-里程碑与执行计划)
9. [测试策略](#9-测试策略)
10. [可观测性要求](#10-可观测性要求)
11. [代码质量与协作约定](#11-代码质量与协作约定)
12. [关键决策记录](#12-关键决策记录)
13. [错误定位手册](#13-错误定位手册)
14. [目标目录结构](#14-目标目录结构)
15. [术语表](#15-术语表)

---

## 1. 为什么要重写：根因诊断

当前系统的症状是：**跑不起来、上电顺序导致崩溃、主机反复 bootstrap、bug 不稳定复现**。
典型现象：给 mcu2 上电后 mcu1 崩溃，主机不停 `REGISTER → ASSIGN /mcu1` 循环。

这些**不是单点 bug，而是执行模型与并发模型的结构性缺陷**。协议设计（mesh envelope / mini9P / cluster 路由）本身是对的、值得保留；错的是它跑在了错误的执行骨架上。下面是四个已定位的根因。

### 1.1 根因 A：从机是裸机 `while(1)` 轮询，却在关中断里解析协议

`pwos-slave/Core/Src/main.c` 的主循环是单线程 `while(1)`，每轮调用 `mesh_node_service_poll_once()`，逐个 UART 端口 `receive_frame`。

而 `pwos-shared/mesh/transport/mesh_uart_transport.c` 的 `stm32_poll_dma_rx`（约 434-471 行）在**主线程**里用 `__disable_irq()` 全局关中断，去执行一个名字带 `_from_isr`、逐字节做 magic 匹配 + 长度解析 + 入队的函数 `stm32_dma_consume_until_from_isr`。

**后果**：端口越多、帧越大、波特率越高，单次关中断窗口越长。mcu2 上电时 mcu1（作为中继）要在多个端口同时承受 REGISTER/ASSIGN/PROBE 突发，关中断窗口一拉长 → DMA 缓冲溢出、字节丢失、帧错位 → 半截帧喂进解析器 → 状态机错乱 → 崩溃。这就是"上电顺序导致崩溃 + 不稳定复现"的来源。

### 1.2 根因 B：ISR 边界划错，DMA 中断回调是空的

`HAL_UARTEx_RxEventCallback`（约 547 行）是**空函数**。真正驱动收包的 IDLE / DMA-TC 中断没有被利用，收包靠主循环轮询 DMA 计数器差值。本该在中断里完成的"组装完整帧并入队"，被推迟到主线程关中断去做——这正是根因 A 关中断搬运的根源。所谓"DMA + 中断"名存实亡。

### 1.3 根因 C：多端口 round-robin + quarantine 把上游路径打断

`mesh_node_service.c` 用 `next_rx_index` 轮询各端口，并有 `quarantine_until_ms` 机制：某端口坏帧太多就隔离一段时间。mcu2 上电冲击 mcu1 时，mcu1 的某个端口可能被打入隔离，导致它在上游路径上"时通时断"——主机间歇收不到/发不出 ASSIGN 回转，mcu2 于是反复 REGISTER。这就是"反复 bootstrap"的直接原因。

### 1.4 根因 D：主机单事务 `dispatch_busy` 独占接收链路

`pwos-master-esp32p4/mesh/mesh_host_runtime.c`（约 65-81 行）用原子 CAS 保护 `dispatch_busy`，但它是**单事务模型**：全系统同一时刻只允许一个 Mini9P 请求独占接收链路。mcu2 上线时主机 poll 任务在处理 mcu2 的 REGISTER/ASSIGN，此刻若 shell/web 任务或 mcu1 的响应到达，会被挡住或错配 → 响应超时 → 触发重发 → 加剧反复 bootstrap。

### 1.5 结论：保留协议，重写执行骨架

| 保留（已验证、值得复用） | 重写（结构性缺陷） |
|---|---|
| mesh envelope 帧格式与编解码 | 从机执行模型：裸机轮询 → FreeRTOS 多任务 |
| mini9P 协议 / client / server / fid 语义 | transport ISR 边界：关中断搬运 → ISR 入队 + 任务消费 |
| cluster 路由算法（Dijkstra 拓扑派生） | 端口管理：静态上下游假设 → 动态角色发现 |
| CRC-16/CCITT-FALSE 校验 | 主机并发：单事务 dispatch_busy → pending 表多在途 |
| local VFS 后端（sys/dev/fs）资源模型 | 控制面/数据面：互相穿透 → 严格隔离 |

> **重写不等于把所有代码推倒重打。它的精确含义是：把状态机、任务边界、模块所有权重新定义清楚，地基（链路层、RTOS 骨架、控制面）从零按新架构写，上层协议逻辑（mini9P、cluster 算法）在新骨架上复用。**

---

## 2. 终态愿景

重构必须从第一行代码就为下面的终态预留接口，避免走到一半推倒。

### 2.1 能力目标

- **任意拓扑的多机集群**：主机不必直连所有从机，从机之间可多跳中继。**转发只部署在 STM32 上**。
- **外设即文件**：主机以 9P 文件读写控制远端外设、读写文件。`cat /mcu1/sys/health`、`echo 100 > /mcu2/motor/speed`。
- **多主多从**：多个 ESP32 主机协同，每台主机管理自己的从机子树；主机之间可互相 RPC 调用、同步拓扑。
- **分布式计算**：主机调用从机的计算资源做并行计算——图像处理、渲染、文本生成（espLLM 是雏形）。计算以 9P 文件语义 + Job 抽象暴露。
- **多链路**：UART 有线骨干为主，未来可扩展 SPI/CAN/USB；主机间走 IP（LAN/WiFi 经路由器）。

### 2.2 用户视角必须保持简单

```bash
# 文件 / 外设
cat /mcu1/sys/health
echo 100 > /mcu2/motor/speed
cp local.bin /mcu3/fs/data.bin

# RPC（不伪装成文件读写）
rpc /mcu1.compute.hash '{"data":"abcd"}'

# 分布式计算
submit --kernel mandelbrot --tiles 16 job.json
```

### 2.3 物理部署（已确认，决定网络架构）

- **ESP32-P4 主机**：**没有 WiFi**，用 **LAN 口连接路由器**。是 leader 候选 + 网关。
- **其他 ESP32 主机**：有 WiFi，连接同一个路由器。
- **STM32F407 从机**：只有 UART/SPI，物理上只能接到某台主机或其他从机，不接入 IP 网络。
- **路由器**：主机间 IP 互通的中转。架构上抽象为 `host_link`，不绑死具体路由器。

> 本轮只支持 **ESP32-P4 主机 + STM32F407 从机**。F411 支线、ESP-LLM 支线暂不纳入（espLLM 留作 M9+ 的计算 kernel 参考）。

---

## 3. 架构总览：双平面模型

系统由两个**正交的网络平面**组成,它们只在 ESP32 主机上汇合。这是理解整个架构的核心。

```text
┌───────────────── 主机间 IP 平面 (host_rpc: TCP + mDNS + CBOR) ──────────────────┐
│                                                                                 │
│   [ESP32-P4 主机]            [ESP32 主机 B]            [ESP32 主机 C]            │
│    LAN ─┐                     WiFi ─┐                   WiFi ─┐                  │
│         └──────────────── 路由器 ───┴────────────────────────┘                  │
│   (leader + 网关)                                                               │
│   · 主机间 RPC                                                                   │
│   · 全局拓扑 / 节点归属同步                                                       │
│   · WebShell / HTTP API 接入                                                     │
│   · STM32 完全不参与本平面                                                        │
└────────┬────────────────────────┬────────────────────────┬─────────────────────┘
         │ UART                    │ UART                    │ UART
┌────────▼────────────────┐ ┌─────▼──────────┐ ┌────────────▼─────────────────────┐
│  有线 mesh 骨干平面 (mesh envelope: 控制面 + mini9P + 计算数据，STM32 中转转发)   │
│                                                                                  │
│  [P4] ─UART─ [mcu1] ─UART─ [mcu2] ─UART─ [mcu3] ...                              │
│              [mcu1] ─UART─ [mcu4] (任意拓扑、多跳、中继)                          │
│   主机 B ─UART─ [mcu5] ...   (每台主机各自一棵 mesh 子树)                         │
└──────────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 两个平面的分工

| | 有线 mesh 骨干平面 | 主机间 IP 平面 |
|---|---|---|
| 成员 | 主机 ↔ STM32 从机 | ESP32 主机 ↔ ESP32 主机 |
| 物理链路 | UART（未来 SPI/CAN） | TCP over LAN/WiFi（经路由器） |
| 帧格式 | mesh envelope (v2) | host_rpc 帧（CBOR over TCP） |
| 承载内容 | 控制面 + mini9P + 计算数据帧 | 主机间 RPC + 拓扑同步 + 节点归属 |
| 谁转发 | **STM32 从机**（中继多跳） | 路由器（IP 层），主机不转发 mesh |
| 关键约束 | STM32 只解 envelope 头转发，不解 mini9P | STM32 完全不可见此平面 |

### 3.2 ESP32-P4 的双重身份

P4 同时是：

1. **mesh leader**：有线骨干的控制面权威，给所辖从机分配地址 lease、计算路由、下发 route update。
2. **IP 网关 / rendezvous**：主机间平面的汇合点。其他主机通过 host_rpc 向它同步拓扑、转发跨主机的从机访问请求。

第一版**单主**时，这个双重身份被简化成一个主机对象，但 `host_coordinator` 与 `host_rpc` 的接口必须从一开始就分开，为多主预留。

### 3.3 跨主机访问从机的调用链（终态）

```text
用户在主机 B 上执行: cat /mcu1/sys/health   （mcu1 实际归 P4 管）
  │
  ▼
主机 B 的 cluster_vfs: 查节点归属表 → mcu1 归 P4
  │
  ▼
主机 B 的 host_rpc client → TCP → P4 的 host_rpc server
  │
  ▼
P4 在自己的 mesh 子树里访问 mcu1（mini9P Tread /sys/health）
  │
  ▼
结果原路返回主机 B
```

单主阶段没有"跨主机"，但 `cluster_vfs` 的"查节点归属"这一步要从一开始就存在（单主时永远命中本机），这样多主时只需填充远端分支。

---

## 4. 分层职责与所有权

### 4.1 架构原则（贯穿全程的硬规则）

1. **单一所有权**：每类状态只能由一个模块拥有，其他模块通过消息或只读查询访问。
2. **控制面优先**：发现、心跳、地址 lease、路由更新，不能被普通文件读写阻塞。
3. **数据面隔离**：mini9P / RPC 请求不能直接驱动链路发现、端口重启或全局路由删除。
4. **从机自治**：从机自己管理端口、邻居、主机候选、端口健康，不依赖固定拓扑。
5. **RTOS 优先**：从机从一开始就按 FreeRTOS 任务拆分，禁止把 mesh 和主业务放进同一个无限循环。
6. **链路不可信**：任何端口都可能噪声、断电、半连接、反复重启；异常端口必须被隔离，不能拖垮其他端口。
7. **协议可演进**：frame type、capability、lease、epoch 必须留扩展空间，支持多主和未来 RPC。
8. **可观测性内建**：每个节点必须能输出端口、任务、队列、路由、会话、错误计数，而不是出问题后临时加日志。
9. **测试先于硬件**：状态机和路由算法必须有 PC 单测，硬件只验证驱动和电气问题。
10. **零拷贝、静态内存**：热路径用固定块池，不在数据面 malloc，不在 ISR 做协议解析。

### 4.2 分层总图

```text
                          主机                                  从机
┌─────────────────────────────────────┐
│ 上层入口                              │
│  WebShell / Serial Shell / Lua /      │
│  HTTP API / RPC Client / Scheduler    │
├─────────────────────────────────────┤
│ 主机资源 API                          │
│  cluster_vfs / rpc_client / job_mgr   │
├─────────────────────────────────────┤
│ 主机间平面            主机 mesh 核心   │
│  host_rpc          host_coordinator   │
│  (TCP/mDNS/CBOR)   topology_db         │
│                    route_planner       │
│                    session_manager     │
├─────────────────────────────────────┤        ┌──────────────────────────────┐
│ link 管理                             │        │ service 运行时 (FreeRTOS task)│
│  uart_link / tcp_link / link_mux      │        │  mini9P server / RPC server /  │
├─────────────────────────────────────┤        │  job worker / 资源注册表       │
│              有线 mesh 骨干 (UART)     │◄──────►├──────────────────────────────┤
└─────────────────────────────────────┘        │ mesh 核心 (FreeRTOS task)      │
                                                │  identity / lease /            │
                                                │  neighbor / route / forwarder  │
                                                ├──────────────────────────────┤
                                                │ link 层 (FreeRTOS task + ISR)  │
                                                │  port_manager / uart_dma /     │
                                                │  frame_parser                  │
                                                ├──────────────────────────────┤
                                                │ 本地资源                       │
                                                │  sys / dev / fs / compute      │
                                                └──────────────────────────────┘
```

### 4.3 模块所有权表

| 模块 | 拥有的状态 | 禁止做的事 |
|---|---|---|
| **从机 link 层** | 每端口 DMA ring、TX queue、端口统计、端口角色 FSM | 不懂 mesh 地址/路由/mini9P；ISR 不解析协议、不进 VFS |
| **从机 port_manager** | 端口角色（UNKNOWN/UPSTREAM/DOWNSTREAM/PEER/QUARANTINED…）、peer uid、健康计数 | 不决定全局路由；端口异常只影响本端口 |
| **从机 mesh 核心** | 本机 identity、地址 lease、邻居表、本地转发表 | 不读写 UART 寄存器；不访问文件系统；等响应时不阻塞控制面 |
| **从机 service 运行时** | mini9P fid 表、RPC pending、job 状态 | 不处理 mesh 控制面；单任务串行访问 littlefs |
| **从机本地资源** | sys/dev/fs/compute 后端数据 | 不直接发 mesh 帧 |
| **主机 host_coordinator** | 全局拓扑 DB、地址分配器、route planner、lease 表、host epoch | 不拥有 WebShell UI / mini9P fid / UART DMA |
| **主机 session_manager** | mini9P attach 状态、RPC stream、pending 表（按 `(mesh_addr, tag)`） | route online ≠ attached；session reset 不删节点/链路 |
| **主机 cluster_vfs** | 节点名↔UID↔归属主机 映射、本地 fd↔远端 fid | 不拥有 next_hop（归 coordinator）；并发访问需加锁 |
| **主机 host_rpc** | 主机间 TCP 连接、mDNS 注册、跨主机 pending | 不封装进 mesh envelope；STM32 不可见 |

---

## 5. 协议栈设计

四套协议各司其职，**不互相伪装**。

```text
┌─ host_rpc 帧 ──────────── 主机间 (TCP/CBOR) ─────────────────┐
│  独立协议，见 5.4。STM32 不可见。                            │
└──────────────────────────────────────────────────────────────┘

┌─ mesh envelope v2 ─────── 有线骨干 (UART) ───────────────────┐
│  magic | ver | hdr_len | type | flags | src | dst | ttl |    │
│  seq | ack | payload_len | hdr_crc | payload_crc | payload    │
│         │                                                     │
│         ├─ LINK_* 链路本地控制帧 (5.2.1)                       │
│         ├─ CTRL_* 全局控制帧     (5.2.2)                       │
│         └─ DATA_* 数据面帧       (5.2.3)                       │
│              ├─ DATA_MINI9P → mini9P 帧 (5.3)                  │
│              ├─ DATA_RPC    → 从机 RPC 帧                      │
│              └─ DATA_JOB / DATA_BULK → 计算任务/大数据         │
└──────────────────────────────────────────────────────────────┘
```

### 5.1 link frame（统一帧头）

所有有线骨干帧共用一个帧头。设计要点：

- **magic + version**：版本字段让 v1（旧 baseline）和 v2 能在同一 UART 共存一段时间，平滑迁移。
- **hdr_len**：头部长度可变，未来加字段不破坏旧解析器。
- **header_crc 与 payload_crc 分离**：头坏和体坏分别可诊断；坏头直接重新同步，坏体可只丢该帧。
- **ttl**：多跳防环。
- **seq + ack**：为未来链路层可靠传输预留（第一版可不启用 ack）。

建议字段（最终以 `pwos-shared/link/pwos_link_frame.h` 为准）：

```text
magic        2 bytes   固定 'M' 'H'
version      1 byte    协议版本，v2=2
hdr_len      1 byte    头部字节数（含 magic 到 payload_crc）
type         1 byte    帧类型（LINK_/CTRL_/DATA_）
flags        1 byte    分片、压缩、加密等标志位预留
src          1 byte    源 mesh 地址
dst          1 byte    目的 mesh 地址（0xFF = 未分配/广播）
ttl          1 byte    剩余跳数
seq          2 bytes   发送序号
ack          2 bytes   确认序号（预留）
payload_len  2 bytes   负载字节数
hdr_crc      2 bytes   头部 CRC-16/CCITT-FALSE
payload_crc  2 bytes   负载 CRC-16/CCITT-FALSE
payload      N bytes   N ≤ MESH_MAX_PAYLOAD_LEN (512)
```

**parser 是增量状态机**，必须满足：逐字节喂入可用、一次喂入整块 DMA buffer 可用、遇噪声能重新同步、半帧不阻塞、长度异常丢弃当前候选帧、payload 中出现 magic 不会错误同步。

### 5.2 mesh envelope v2 帧类型

#### 5.2.1 链路本地控制帧（无需全局地址，适合未分配阶段）

```text
LINK_HELLO          周期广播：uid, boot_id, port_id, caps, role, link_type, mtu
LINK_HELLO_ACK      回应 HELLO，确认链路双向可达
LINK_HEARTBEAT      链路保活
LINK_ERROR          链路级错误通告
```

#### 5.2.2 全局控制帧（需要 mesh 地址 + coordinator epoch）

```text
CTRL_NODE_REGISTER  节点注册：uid, boot_id, caps, upstream_port
CTRL_ADDR_ASSIGN    地址分配：addr, lease_epoch, lease_ms
CTRL_LEASE_RENEW    续租请求
CTRL_LEASE_ACK      续租确认
CTRL_LINK_STATE     链路状态上报：local, peer, metric, flags
CTRL_ROUTE_UPDATE   路由下发：dst, next_hop, metric, route_version, action
CTRL_HOST_ADVERTISE 主机通告（多主预留）：host_uid, epoch, priority, cluster_id
CTRL_TIME_SYNC      时钟同步
CTRL_ERROR          控制面错误
```

身份模型（关键）：

- `node_uid`：硬件稳定身份，重启/重连/重命名不变。
- `boot_id`：每次启动变化，用来识别"是不是真的重启了"。
- `addr`：运行时短地址，coordinator 租约分配，可回收。
- `lease_epoch`：地址租约版本，防止旧 route 覆盖新 route。

#### 5.2.3 数据面帧

```text
DATA_MINI9P   文件语义（attach/walk/open/read/write/stat/clunk）
DATA_RPC      从机 RPC 方法调用（走 mesh，区别于主机间 host_rpc）
DATA_JOB      计算任务下发 / 状态 / 结果
DATA_BULK     大数据块传输（图像帧等）
```

### 5.3 mini9P（复用现有实现）

mini9P 协议本体、client、server、fid 表语义**全部保留**，只是：

- 在线缆上不再直接出现，而是作为 `DATA_MINI9P` 的 payload。
- 主机侧从单事务改为 session_manager 管理（见 7.3）。
- 从机侧从主循环内联改为独立 service 任务处理（见 6.3）。

关键约束：中继 STM32 只看 mesh envelope 头转发，**绝不解析 mini9P 内容**。

### 5.4 host_rpc（主机间，新增，独立于 mesh）

主机间 RPC 是**独立协议**，不复用 mesh envelope，STM32 完全不参与。

- **传输**：TCP（可靠、有序）。
- **发现**：mDNS（解决"谁是 leader、其他主机在哪、各主机管哪些从机"）。
- **编码**：CBOR（MCU 友好、流式、比 JSON 省一半带宽）。
- **语义**：unary / server-streaming / fire-and-forget / cancel / deadline。

host_rpc 帧（CBOR map）：

```text
{
  v:          协议版本
  call_id:    调用 ID
  kind:       REQUEST / RESPONSE / CANCEL / STREAM_CHUNK / STREAM_END
  service:    服务名，如 "cluster" / "topology" / "compute"
  method:     方法名，如 "read_node" / "sync_topo" / "submit_job"
  deadline:   截止毫秒
  status:     （响应）状态码
  payload:    CBOR 编码的方法参数 / 结果
}
```

主机间核心方法（第一版）：

```text
cluster.read_node(node_name, path)        跨主机读远端从机文件
cluster.write_node(node_name, path, data) 跨主机写
topology.sync(node_table, link_table)     leader → follower 拓扑同步（多主预留）
topology.whoowns(node_name)               查节点归属哪台主机
host.advertise(host_uid, epoch, priority) 主机互相发现（多主预留）
```

### 5.5 计算抽象（9P 文件语义 + Job）

计算资源以 **9P 文件语义**暴露，保持"一切皆文件"的一致性：

```text
/mcuN/compute/caps           读：节点算力描述（cpu class, free mem, kernels, max payload, load）
/mcuN/compute/load           读：当前负载
/mcuN/compute/jobs           写：提交 job 描述
/mcuN/compute/jobs/<id>/status   读：job 状态
/mcuN/compute/jobs/<id>/result   读：结果（大结果走 DATA_BULK 分块）
/mcuN/compute/jobs/<id>/log      读：执行日志
```

主机侧有 `job_manager` 负责：拆分子任务、按 capability 调度、追踪状态、收集结果、重试/迁移。从机侧 `compute_worker` 是独立低优先级任务，暴露 kernel 列表、接收 chunk、上报进度，**不阻塞 mesh 控制面**。

第一批 kernel（可验证、结果可肉眼对比）：`hash`、`vector_add`、`small_matmul`、`mandelbrot_tile`。LLM 推理切片留到这套链路稳定之后。

---

## 6. 从机架构（STM32F407 + FreeRTOS）

### 6.1 为什么必须上 FreeRTOS

根因 A/B/C 的本质都是"单线程裸机里塞了太多互相干扰的职责"。解决方式是把职责拆成独立任务，用队列通信，让任何一个端口/请求的阻塞都无法拖死其他职责。F407 有 128K 主 SRAM + 64K CCMRAM，跑 FreeRTOS 绰绰有余。

**采用原生 FreeRTOS API**（`xTaskCreate` / `xQueueSend` / `xSemaphore`），不经 CMSIS-RTOS2 抽象——少一层，且与 ESP32-P4 的 IDF（同为 FreeRTOS）两端一致。

### 6.2 任务模型

| 任务 | 优先级 | 职责 | 禁止 |
|---|---|---|---|
| `link_rx_task` | 高 | 等 ISR 信号，从每端口取完整帧，投递 ingress 队列 | 不解析业务、不进 VFS |
| `link_tx_task` | 高 | 串行化每端口发送，处理 TX 超时 | 不等待业务结果 |
| `port_mgr_task` | 高 | 端口发现、角色 FSM、健康、隔离 | 不决定全局路由 |
| `mesh_ctrl_task` | 高 | REGISTER/ASSIGN/HELLO/HEARTBEAT/ROUTE_UPDATE、转发非本机帧 | 不做长计算、不做文件 IO |
| `service_task` | 中 | mini9P server / RPC server 请求处理（单线程串行） | 不处理 mesh 控制面 |
| `app_task` | 中低 | 板级主业务、传感器、控制逻辑 | 不阻塞 mesh |
| `compute_worker_task` | 低 | 长计算任务 | 不占高优先级锁 |
| `diag_task` | 低 | 统计、日志、watchdog feed | 不改路由 |

> 中继转发（mcu1 转发 mcu2↔主机 的帧）由 `mesh_ctrl_task` 直接完成：查本地转发表、ttl-1、丢给 `link_tx_task`，**不进 service 队列**——转发路径绝不唤醒业务层。

### 6.3 link / service 隔离（你最初的诉求）

两层是两个独立任务,唯一通道是两条队列 + 帧块池:

```text
                  ┌──────────── frame_pool (固定块池, 块=526B) ────────────┐
                  │                                                         │
ISR (DMA/IDLE) ──► 取空块填完整帧 ──► link_rx 队列 ──► mesh_ctrl_task        │
                                                          │ 本机 mini9P T*   │
                                                          ▼                  │
                                              mini9p_request_queue           │
                                                          │                  │
                                                  service_task               │
                                                  (m9p_server + VFS, 单线程)  │
                                                          │ 取第二个块作响应  │
                                                          ▼                  │
                                              mini9p_response_queue          │
                                                          │                  │
                                              mesh_ctrl_task 封 envelope 发出 │
                                                          └──── 归还块 ───────┘
```

关键设计：

- **零拷贝 + 固定块池**：块大小 = `MESH_PROCESSER_FRAME_CAP` = 526 字节（= 14 头 + 512 负载）。队列里传**块指针**，不拷贝帧体。块池容量按"每端口 DMA 排队 4 帧 × 端口数 + service 在途若干"估算，约 24 块 ≈ 13KB，放主 SRAM。
- **每个在途 mini9P 请求需 2 个块**：一个装请求（service 处理期间只读），一个装响应。因为 TWRITE 会把请求里的 data 指针传给后端 write，同时响应在另一个块构建——两者不能复用同一块。
- **service 任务单线程 = 天然串行**：mini9P 的 fid 表、littlefs 都不是线程安全的；单消费者串行访问天然安全，**不需要额外 mutex**。这是用"单线程 service 任务"换掉"到处加锁"的关键简化。
- **诊断查询跨任务**（`/sys/routes` 读 link 层状态）：用快照 + mutex，或 service 经查询队列向 mesh_ctrl_task 要数据，不直接读对方内部结构。

### 6.4 transport ISR 重写（修根因 A/B）

- **ISR 内（IDLE / DMA-TC 回调）**：只做"消费 DMA 环形缓冲新增字节 → 增量解析 → 组装完整帧 → 取空块拷入 → `xQueueSendFromISR` 通知 link_rx_task"。ISR 临界区用 `taskENTER_CRITICAL_FROM_ISR`（只屏蔽同级及以下中断），**绝不用 `__disable_irq` 全局关中断**。
- **`HAL_UARTEx_RxEventCallback` 实现真正逻辑**（当前是空的）。
- **主线程不再轮询 DMA 计数器、不再关中断搬运**：`receive_frame` 改为 `xQueueReceive` 阻塞/带超时。
- **NVIC 优先级**：UART/DMA 中断优先级数值必须 ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY`（用了 FromISR API 的硬性要求）。DMA RX buffer 用 `__attribute__((section(".ccmram")))` 放 CCMRAM，省主 SRAM。

### 6.5 端口自发现（修根因 C：去掉静态上下游假设）

板级代码**只提供候选 UART 列表**，不指定谁是上游：

```c
static const pwos_port_desc_t pwos_board_ports[] = {
    { .id = 0, .name = "USART1", .huart = &huart1 },
    { .id = 1, .name = "USART2", .huart = &huart2 },
    { .id = 2, .name = "USART3", .huart = &huart3 },
    { .id = 3, .name = "UART4",  .huart = &huart4 },
    { .id = 4, .name = "USART6", .huart = &huart6 },
};
```

端口角色 FSM：

```text
DISABLED → PROBING → LINK_UP → {HOST_CANDIDATE → UPSTREAM | DOWNSTREAM | PEER}
                              → SUSPECT → QUARANTINED → PROBING
```

发现流程：每端口周期发 `LINK_HELLO(uid, boot_id, caps, port_id)` → 收到对端 HELLO/ACK → 按 capability 判角色（对端含 `ROLE_COORDINATOR` → host candidate；普通节点 → peer/downstream）→ coordinator client 选主机 → 发 `CTRL_NODE_REGISTER` → 收 `CTRL_ADDR_ASSIGN` 进入 ASSIGNED。

**端口隔离只影响本端口**：某端口持续 CRC 错/版本错/心跳丢失 → 标 SUSPECT → 超阈值 QUARANTINED → 一段时间后回 PROBING。**绝不清空全局路由，绝不重启整个 mesh**。这是根因 C 的根治。

### 6.6 从机启动序列

```text
board_init (CubeMX: 时钟/GPIO/UART/DMA/FreeRTOS)
  → frame_pool_init / queues_init
  → 创建所有任务
  → osKernelStart / vTaskStartScheduler
      → port_mgr 打开所有候选端口，开始 PROBING
      → mesh_ctrl 等待 ASSIGN
      → service 等待请求
      → app/diag 独立运行
```

删除 `main.c` 里的 `while(1) mesh_node_service_poll_once()` 超级循环。

---

## 7. 主机架构（ESP32-P4 + 主机间平面）

### 7.1 主机模块拆分

把现在臃肿的 `mesh_host_runtime`（1291 行）拆成清晰的模块：

```text
host_coordinator/        有线 mesh 骨干的控制面权威
  coordinator.c          注册/lease/topology 事件入口
  address_allocator.c    地址分配与回收
  topology_db.c          全局拓扑图
  route_planner.c        从拓扑派生路由、下发 ROUTE_UPDATE
  lease_table.c          地址租约表

host_sessions/           数据面会话管理
  session_manager.c      pending 表 (按 mesh_addr, tag)、deadline、重试、tag 匹配

host_api/                上层资源 API
  cluster_vfs.c          统一命名空间、节点名↔UID↔归属主机、本地 fd↔远端 fid
  rpc_client.c           从机 RPC 客户端
  job_manager.c          分布式计算调度

host_rpc/                主机间 IP 平面（新增）
  host_rpc_server.c      TCP server，处理其他主机的 RPC
  host_rpc_client.c      TCP client，调用其他主机
  host_discovery.c       mDNS 注册与发现
  host_rpc_codec.c       CBOR 编解码

host_link/               链路抽象
  uart_link.c            到从机的 UART（mesh 平面）
  tcp_link.c             到其他主机的 TCP（IP 平面）
  link_mux.c             统一收发抽象
```

### 7.2 主机控制面 / 数据面流转

```text
控制面:
  link_rx → frame_decode → coordinator 事件队列
  coordinator → topology_db / lease_table / route_planner
  route_planner → route_update 队列 → link_tx

数据面（本地从机）:
  shell/web/lua → cluster_vfs → session_manager
  session_manager → mesh_data 队列 → route lookup → link_tx
  link_rx → 响应 → 按 (mesh_addr, tag) 匹配 pending → 唤醒等待者

数据面（跨主机从机，多主阶段）:
  cluster_vfs → 查归属 → host_rpc_client → TCP → 远端主机
```

### 7.3 主机并发模型重写（修根因 D）

把单事务 `dispatch_busy` 换成 **pending 表 + tag 匹配**：

- poll 任务是**唯一接收消费者**：收帧 → 控制帧给 coordinator，mini9P R* 帧按 `(src mesh_addr, tag)` 查 pending 表唤醒对应等待者。
- `mesh_host_runtime_client_request` 改为：分配 tag → 插 pending 表 → 发 T* → `xSemaphoreTake(done, deadline)` 阻塞等 → poll 任务匹配到 R* 后 `xSemaphoreGive`。
- **tag 全局唯一**：`next_tag` 从每 client 独立改为 runtime 全局递增（或 pending 表用 `(slot, tag)` 复合键）。
- **cluster_vfs 加锁**：`g_routes` / `g_open_files` 当前无锁，放开并发后两个上层任务（shell/web）会在两次 round-trip 之间竞争，必须加 mutex。

### 7.4 主机间平面与多主预留

- 第一版**单主**：`host_coordinator` 是唯一 leader，`host_rpc` server 已启动但只有本机连，`cluster_vfs` 查归属永远命中本机。
- 多主演进：`CTRL_HOST_ADVERTISE`（mesh 内）+ `host.advertise`（IP 内）让主机互相发现；leader 由 `(epoch, priority, host_uid)` 选出（P4 因有 LAN 接入有天然 bias）；follower 通过 `topology.sync` 订阅 leader 拓扑；节点只接受当前 leader 的 lease 和 route。
- 角色：`LEADER`（分配地址/路由）/ `FOLLOWER`（同步，可接管）/ `OBSERVER`（只读）/ `CLIENT`（只发请求）。

---

## 8. 里程碑与执行计划

### 8.1 总体路线

| 里程碑 | 目标 | 阶段门（达成才进下一步） |
|---|---|---|
| **M0** | 冻结 baseline + 仓库清理 | 旧代码可构建可回退；F411/build 产物清理；问题档案建立 |
| **M1** | link frame/parser 在 PC 上稳定 | 噪声/半帧/CRC/乱序/随机 1MB 单测全过 |
| **M2** | 从机 FreeRTOS 骨架 + UART DMA/ISR | 单板 30 分钟无 HardFault；噪声输入不死机；任务 heartbeat 正常 |
| **M3** | 从机 port_manager 自发现 | 任意端口接 host 都识别 upstream；mcu2 上电不影响 mcu1 本地 |
| **M4** | mesh 控制面（lease/topology/route） | `P4–mcu1–mcu2` 稳定看到两节点；mcu2 reboot 不影响 mcu1 |
| **M5** | mini9P 数据面 + session_manager | `ls`/`cat` 不被拓扑变化拖死；错误类型明确 |
| **M6** | 主机并发模型 + cluster_vfs 加锁 | 并发 9P 请求不互斥不错配 |
| **M7** | WebShell/LAN + 可观测性 | WebShell 不因单命令失败断线；`/sys/*` 能定位现场 |
| **M8** | 从机 RPC（DATA_RPC） | unary/cancel/deadline 可用；不阻塞控制面 |
| **M9** | 分布式计算（job + worker） | mandelbrot/matmul job 下发、进度、结果、取消可用 |
| **M10** | 主机间平面 + 多主 alpha | host_rpc 跨主机读从机；leader/follower 基础可用 |

**M0–M7 是第一阶段 MVP**，必须先稳。M8–M10 是高级能力，MVP 不稳前不要混入。

### 8.2 每个里程碑的展开

每个里程碑按统一模板：**做什么 / 测什么 / 通过标准 / 失败怎么办**。

---

#### M0：冻结 baseline 与仓库清理

**做什么**
1. 确认在 `refactor` 分支工作（已是）。
2. 记录当前可构建状态：master `idf.py build`、slave `cmake --preset F407Debug && cmake --build --preset F407Debug`。
3. 记录硬件 baseline 行为：`P4–mcu1` 单板、`P4–mcu1–mcu2`（mcu2 先不供电 / 再供电），分别记录 `mesh`、`ls /`、`cat /mcu1/sys/health`、`cat /mcu1/sys/routes` 的输出。
4. **删除 F411 支线**：`git rm -r pwos-slave-stm32f411/`，清理 README/architecture.md/CONTEXT.md 中 F411 引用。
5. **仓库瘦身**：`pwos-master-esp32p4/build/`（182MB）和 `managed_components/`（109 个被跟踪文件）移出 git，修正 `.gitignore` 覆盖所有子工程 build 目录。
6. 建问题档案 `docs/logs/refactor/M0-baseline.md`，记录第 1 章四个根因的实测复现。

**测什么**：master build、slave build、emulator smoke test 至少各成功一次并留记录。

**通过标准**：`git status` 干净；baseline 记录存在；四个根因有实测复现记录。

**失败怎么办**：若构建本身失败，只修构建，单独提交，不碰 mesh 行为。

---

#### M1：link frame 与 parser（PC 纯软件）

新建 `pwos-shared/link/`：`pwos_link_frame.{h,c}`、`pwos_link_parser.{h,c}`、`pwos_link_crc.{h,c}`、`tests/`。

**做什么**
1. 按 5.1 定义统一帧头与编解码。
2. parser 增量状态机（5.1 的六条要求）。
3. 标准化错误码：`PWOS_OK / E_BAD_MAGIC / E_BAD_VERSION / E_BAD_LENGTH / E_BAD_CRC / E_NO_MEMORY / E_QUEUE_FULL / E_DEADLINE / E_LINK_DOWN / E_NO_ROUTE`。底层不传播裸 `-13`，到上层命令再映射 `EAGAIN/ENOENT`。
4. `tests/golden/` 存固定输入输出，防止协议改坏没人知道。

**测什么**（PC 单测必覆盖）：空输入、单字节、整帧一次、连续两帧、噪声+帧、帧+噪声、半帧分两次、payload=0、payload=最大、payload_len 偏大、偏小、头 CRC 错、体 CRC 错、版本不支持、payload 含 magic 不误同步、1MB 随机噪声不越界/不死循环/内存不增长。

**通过标准**：parser 不依赖 HAL/IDF；无阻塞等待；所有单测过；任意错误输入状态可恢复；注释解释帧格式、CRC 范围、重新同步策略。

**失败怎么办**：只修 `pwos-shared/link/`，不碰 STM32/ESP32 业务。

---

#### M2：从机 FreeRTOS 骨架 + UART DMA/ISR（修根因 A/B）

**做什么**
1. CubeMX 启用 FreeRTOS（原生 API）；若 FreeRTOS 占用 SysTick，HAL timebase 改用一个普通 TIM。UART 开 DMA RX/TX + IDLE 中断 + NVIC。
2. 新建 `pwos-slave/User/rtos/`（`pwos_tasks.{h,c}`、`pwos_queues.{h,c}`）、`pwos-slave/User/os/frame_pool.{h,c}`、`pwos-slave/User/drivers/uart_dma_port.{h,c}`。
3. 实现 6.4 的 ISR 重写：ISR 入队、主线程不关中断。
4. 建 6.2 的任务集（业务先 mock），所有任务经队列通信，每任务有 heartbeat。

**测什么**
- PC：队列满不泄漏；帧块池 alloc/free 正确；deadline 到期清理。
- 单板：上电 30 秒无 HardFault；不接任何 UART，diag 仍 heartbeat；USB-TTL loopback 发噪声不死机；高频输入不丢 parser 状态；TX queue 满返回 `E_QUEUE_FULL` 不阻塞。

**通过标准**：没有业务逻辑留在主 `while(1)`；任一 UART 灌垃圾不影响 service heartbeat；不接 host 也能稳定运行；`grep __disable_irq pwos-shared/mesh/transport` 只剩 FreeRTOS 的 mask。

**失败怎么办**：停在单板 RTOS+DMA，不接 mesh。HardFault/栈溢出/DMA 越界没解决不进 M3。开 `configCHECK_FOR_STACK_OVERFLOW=2`。

---

#### M3：从机 port_manager 自发现（修根因 C）

**做什么**：按 6.5 实现端口角色 FSM、`LINK_HELLO`/`LINK_HELLO_ACK`、端口隔离、`/sys/ports`。board 层只给候选 UART 列表，不写死上下游。

**测什么**
- PC FSM：空口循环 PROBING 不进 UPSTREAM；host 接入 PROBING→HOST_CANDIDATE→UPSTREAM；node 接入 PROBING→PEER；host 断开 UPSTREAM→SUSPECT→PROBING；噪声口 PROBING→QUARANTINED→PROBING 不影响他口；两 host 同时接入都成 candidate 不崩；peer 重启 boot_id 变旧 session 清理。
- 硬件：host 接任意 UART 都识别 upstream；mcu2 不供电时 mcu1 upstream 稳定；mcu2 反复供电 20 次 mcu1 upstream 不重置；拔 mcu2 后 mcu1 `/sys/health` 仍可读。

**通过标准**：`cat /mcu1/sys/ports` 能看到所有端口状态；mcu2 端口异常不影响 mcu1 upstream；主机不再反复看到 mcu1 bootstrap（除非 mcu1 真重启）。

**失败怎么办**：mcu2 上电仍导致 mcu1 upstream 消失时，先看 `/sys/ports`：upstream 也变 SUSPECT → 查电气/供电/共地/DMA 越界；只 mcu2 端口异常但 upstream 被删 → port_manager 或 mesh_ctrl 所有权错误；task heartbeat 停 → 调度/阻塞问题。

---

#### M4：mesh 控制面（lease / topology / route）

新模块建议放 `pwos-shared/mesh2/`（与旧 mesh 并行，M5 稳定后再替换）+ `pwos-master-esp32p4/host_coordinator/` + `pwos-slave/User/mesh2/`。

**做什么**：身份模型（uid/boot_id/addr/lease_epoch）；host 分配地址 lease 三次握手；节点周期续租，lease 未确认不承载数据面；拓扑上报 link state；**路由只在 host 计算**（host owns topology，下发 next-hop，node 只维护本地转发表）；TX 分两级优先级（控制面高于数据面）。

**测什么**
- PC：单节点注册；两节点链式；mcu1 先 mcu2 后；mcu2 经 mcu1 注册；mcu2 重启 host 保持 mcu1 地址只更新 mcu2 boot_id；mcu1 重启 host 删经 mcu1 到 mcu2 的旧路由；重复 REGISTER 不重复建 `/mcu1`；REGISTER 丢包重试不产生重复节点；lease 过期进 limited 模式；route update 乱序时旧 epoch 不覆盖新 route。
- 硬件 `P4–mcu1`：`mesh` 显示 host local，`ls /` 显示 `mcu1/`，复位 mcu1 不出现重复 `/mcu1`。
- 硬件 `P4–mcu1–mcu2`：`ls /` 显示两节点，`mesh` 看到两条路由，mcu2 复位/断电 mcu1 保持在线。

**通过标准**：`mesh` 能区分 node online / link online / route installed / session attached；不再出现"显示 online 但 route 没了"；30 分钟控制面压测无重复挂载、无地址漂移；mcu2 link down 不删 mcu1 的 lease。

**失败怎么办**：不要先改 mini9P。先查 lease 是否存在、route epoch 是否正确、upstream 是否稳定、link state 是否被错误广播、host 是否把 transient link 当 node down。

---

#### M5：mini9P 数据面 + session_manager

**做什么**：引入 session_manager（per 目的节点维护 session id / peer boot_id / max inflight / next tag / pending 表 / deadline / retry）；mini9P client 不直接碰 transport，走 `cluster_vfs → mini9p_client → session_manager → mesh_router → link_tx`；明确超时（本地 `/sys/*` 立即返回不等链路；一跳 500-800ms；两跳 1000-1500ms；deadline 到才返回 `E_DEADLINE`，无 route 立即 `E_NO_ROUTE` 不伪装 EAGAIN）；fid 生命周期（node boot_id 变清该 node 所有 fid；失败路径释放本地资源）。

**测什么**
- PC：正常 attach→walk→open→read→clunk；Twalk 超时后 pending 清理；Topen 失败不漏 fid；tag 不匹配丢弃；旧 boot_id 响应丢弃；route down 新请求立即 `E_NO_ROUTE`；queue full 返回 `E_QUEUE_FULL`；control frame 在 data queue 堵塞时仍可发。
- Host VFS 测试 + 硬件：`cat /mcu1/sys/health` 循环时给 mcu2 上电/拔电；`cat /mcu2/sys/health` 时复位 mcu2；100 次 `ls /` 不丢 mcu1。

**通过标准**：mcu2 上下电时 `cat /mcu1/sys/health` 稳定；`cat /mcu2` 失败时错误能说明是 no route/deadline/link down/reboot；不再出现"很快 EAGAIN 但根本没等到 deadline"。

**失败怎么办**：先确认错误类型——立即失败查 route/session 前置；等到 deadline 失败查下游 link/forwarding；mcu1 也失败查端口隔离和所有权，别先怀疑 mini9P server。

---

#### M6：主机并发模型 + cluster_vfs 加锁（修根因 D）

**做什么**：按 7.3 把 `dispatch_busy` 换成 pending 表 + `(mesh_addr, tag)` 匹配；tag 全局唯一；poll 任务做唯一接收消费者；`g_routes`/`g_open_files` 加 mutex。

**测什么**：两个 shell/web 任务同时 `cat /mcu1/...` 和 `cat /mcu2/...` 互不阻塞、不串响应；mcu2 上电时主机并发处理 REGISTER + mcu1 响应不触发反复 bootstrap；route 发现/失效与 open file 分配在并发下无竞争。

**通过标准**：并发 9P 请求不互斥、响应不错配、bootstrap 收敛。

**失败怎么办**：响应错配 → 查 tag 键是否全局唯一、是否同时校验 src 地址；状态错乱 → 查 cluster_vfs 锁覆盖是否完整。

---

#### M7：WebShell / LAN + 可观测性

**做什么**：WebShell command 执行与 websocket IO 分离（ws 任务只收发，命令进 command 队列，结果异步写回，每客户端独立 session）；shell 命令有 deadline，超时返回文本但不断连；`net status` 显示 IP/mDNS/HTTP/ws client 数，LAN 断连重连不重置 mesh runtime；实现 10 章全部 `/sys/*` 与故障注入命令。

**测什么**：浏览器连续命令不断线；两浏览器并发输出不串；`cat /mcu2` 超时不断线；刷新后旧 session 清理；30 分钟 idle 不断线；fault drop/delay/corrupt 注入后控制面恢复。

**通过标准**：单命令失败不关 websocket；mesh 日志与 WebShell 输出不互相污染；每次异常都有计数器可查。

---

#### M8–M10（高级能力，MVP 稳定后）

- **M8 从机 RPC**：按 5.4 的 `DATA_RPC` 实现 unary/streaming/fire-and-forget/cancel/deadline；从机注册 RPC service；与 mini9P 共用 session_manager 但 pending 分类型。
- **M9 分布式计算**：host `job_manager` + 从机 `compute_worker`（独立任务）；job 生命周期 CREATED→QUEUED→ASSIGNED→RUNNING→DONE/FAILED/CANCELLED/LOST；第一批 kernel hash/vector_add/matmul/mandelbrot；job 运行不阻塞 mesh 控制面，节点断线 job 标 LOST 可重试。
- **M10 主机间平面 + 多主**：实现 `host_rpc`（TCP+mDNS+CBOR）；`cluster.read_node` 跨主机读从机；`CTRL_HOST_ADVERTISE` + `host.advertise` 主机发现；leader 选举 `(epoch,priority,host_uid)`；`topology.sync` leader→follower；节点只接受当前 leader 的 lease/route。

### 8.3 代码落地顺序（开 commit/PR 的顺序）

```text
1. docs:               本计划书（已完成）
2. shared-link:        frame/parser/crc + PC 单测           (M1)
3. slave-rtos:         FreeRTOS 骨架 + frame_pool + 队列     (M2)
4. slave-uart-dma:     UART DMA driver + ISR + loopback 测试  (M2)
5. slave-port-manager: 端口 FSM + /sys/ports                 (M3)
6. shared-mesh-control:identity/lease/topology/route + PC 测  (M4)
7. host-coordinator:   topology DB + 地址分配                 (M4)
8. node-control:       node register/lease/link state         (M4)
9. mesh-forwarder:     数据面转发                             (M4)
10. mini9p-session:    mini9P adapter + session_manager       (M5)
11. host-concurrency:  pending 表 + cluster_vfs 加锁          (M6)
12. webshell-async:    WebShell 命令队列 + deadline           (M7)
13. observability:     /sys/tasks /routes /sessions /log      (M7)
14. slave-rpc / job / host-rpc ...                            (M8-M10)
```

任何一步失败都不要跳到后面的业务功能。

---

## 9. 测试策略

### 9.1 测试层级

1. **协议编解码单测**：link frame、mesh envelope v2、mini9P、CBOR。
2. **link parser 单测**：随机噪声、半帧、CRC 错、连续帧、1MB 模糊输入。
3. **端口 FSM 单测**：所有状态转换 + 隔离恢复。
4. **coordinator client 单测**：主机选择、lease 续约。
5. **host coordinator 单测**：地址分配、route 规划、epoch 防覆盖。
6. **session 单测**：超时、重试、tag 匹配、boot_id 失配、cancel。
7. **job manager 单测**：调度、节点断线、重试、cancel、结果分块。
8. **硬件 smoke test**：单主单节点、单主链式两节点、下游反复上电不影响上游、下游噪声口隔离、主机断开重连。

### 9.2 硬件验收脚本（MVP 完成时手动跑完）

**构建**：master `idf.py build`；slave `cmake --preset F407Debug && cmake --build --preset F407Debug`；host VFS PC 测试。

**单 mcu1**（`P4–mcu1`）：`cat /mcu1/sys/health` 连续 100 次成功；`mesh` 路由稳定；无重复 register 风暴。

**链式双 mcu**（`P4–mcu1–mcu2`）：`ls /` 显示两节点且地址不同；mcu1 upstream 是 host，downstream/peer 指向 mcu2；任一 `cat` 失败时错误类型准确。

**mcu2 电源扰动**（关键回归）：保持 `P4–mcu1` 稳定，每秒 `cat /mcu1/sys/health`，给 mcu2 反复上电/断电 20 次。**mcu1 health 不能因 mcu2 上电持续失败；mcu1 upstream 不能重置；mcu2 端口可进 SUSPECT/QUARANTINED 但不影响 mcu1 本地资源；主机不出现 mcu1 重复注册风暴**。这是根因 A/B/C/D 全部消除的最终判据。

**WebShell**：两浏览器并发命令不串；mcu2 复位时 `cat /mcu1` 仍稳定；命令失败不断线。

### 9.3 故障注入（debug build）

```text
fault drop port <id> <percent>     按比例丢包
fault delay port <id> <ms>         注入延迟
fault corrupt port <id> <percent>  按比例损坏
fault down/recover port <id>       强制断/恢复端口
fault reboot-self                  自重启
```

注入后必须：控制面能恢复、故障只影响目标端口、恢复不需整板重启、每次异常有计数器可查。

---

## 10. 可观测性要求

### 10.1 从机 `/sys` 接口

```text
/sys/health     ok/fail, uptime, free heap, boot_id, addr, role
/sys/tasks      每任务: name, priority, stack high water mark, last heartbeat, queue depth
/sys/ports      每端口: id/name, role/state, peer uid/addr, rx/tx, crc errors, hb miss, quarantine until
/sys/links      链路: 是否双向、metric、last seen
/sys/neighbors  邻居表
/sys/routes     dst, next_hop, port, metric, epoch, owner
/sys/sessions   peer, boot_id, inflight, next tag, last error, deadline count
/sys/queues     每队列深度、丢弃计数
/sys/log        最近 N 条关键事件
/sys/build      固件版本、构建时间
```

### 10.2 主机 `/host/sys` 接口

```text
/host/sys/health    /host/sys/links     /host/sys/topology
/host/sys/routes    /host/sys/sessions  /host/sys/web      /host/sys/log
```

可观测性是内建要求,不是出问题后临时加。`/sys/ports`、`/sys/routes`、`/sys/sessions` 必须能独立解释任何现场状态(见第 13 章)。

---

## 11. 代码质量与协作约定

### 11.1 你（项目负责人）的工作方式

本项目是嵌入式多机联调,**全部用 AI 生成代码、出 bug 后无人能修**是最大风险。约定:

- **协议层（link frame/parser、mesh control、host_rpc 编解码）必须你自己写或逐行审**——这是地基,bug 会传染到所有上层,且最难调。
- **驱动层（UART DMA、port FSM）可用 AI 生成草稿,但你要能画出状态机图、解释每个状态转换**——"状态机必须有图或表"既是文档要求,也是检验你是否真懂的标准。
- **每步的 PC 单测用例由你设计**（噪声/半帧/乱序/边界）,AI 写实现。测试用例反映你对边界条件的理解,是"你是否真看懂"的试金石。
- **严格按 M0→M10 顺序,不跳步**。现在的 bug 正是因为在没打牢的地基上叠业务。
- **使用 AI 写的代码,合并前你必须能完整复述它的控制流和所有权**。复述不了就不合并。

### 11.2 模块合并前必须满足

- README 或模块注释说明：职责、非职责、状态机。
- 公开函数注释说明：输入、输出、错误码、**线程上下文**（在哪个任务/ISR 调用）。
- 状态机有图或表。
- 协议变更必须带测试。
- 不在 ISR 调用业务逻辑;不跨层直接访问内部结构体;不用全局变量绕过模块 API。
- commit/PR 描述包含:改了哪个模块、这个模块拥有什么状态、影响哪些任务/队列、如何测试。

### 11.3 提交纪律

- 控制面和数据面分开提交,不在同一提交里同时改 UART 驱动、路由、mini9P、WebShell。
- 每个里程碑有验收记录,放 `docs/logs/refactor/<里程碑>.md`。
- 新协议、新状态机先 PC 单测跑通再接硬件。

---

## 12. 关键决策记录

| # | 决策 | 选择 | 理由 |
|---|---|---|---|
| D1 | 目标平台 | ESP32-P4 主机 + STM32F407 从机 | 砍 F411 与 ESP-LLM 支线,聚焦。espLLM 留作 M9+ kernel 参考 |
| D2 | 从机 OS | 原生 FreeRTOS API | 少一层 CMSIS 抽象;与 P4 的 IDF 同为 FreeRTOS,两端一致 |
| D3 | link/service 隔离 | 独立任务 + 消息队列 | 任一端口/请求阻塞不拖死其他职责;service 单线程天然串行免锁 |
| D4 | 帧内存管理 | 零拷贝 + 固定块池(526B) | 无动态分配、可预测、稳定;每在途请求 2 块 |
| D5 | 网络平面 | 双平面:有线 mesh 骨干 + 主机间 IP | STM32 只转发 mesh,不参与主机间;P4 作网关 |
| D6 | 主机间通信 | TCP + mDNS + 独立 host_rpc 协议 | 不复用 mesh envelope;语义清晰、带宽高 |
| D7 | 计算抽象 | 9P 文件语义 + Job | 保持"一切皆文件"一致性;mesh 层零改动 |
| D8 | 多主控制面 | 单主起步,预留多主 | 第一版复杂度可控;接口从一开始分开 |
| D9 | 协议兼容 | v2 新协议,v1 仅作 baseline 回退 | 旧控制面太弱(无 lease/epoch);magic/version 兼容共存 |
| D10 | RPC 编码 | CBOR | MCU 友好、流式、比 JSON 省一半带宽 |
| D11 | 计算 demo | mandelbrot tile + 小矩阵乘 | 可验证分片/合并/进度;结果可肉眼对比 |
| D12 | 主机并发 | pending 表 + (mesh_addr,tag) 匹配 | 替换单事务 dispatch_busy;支持多在途 |

---

## 13. 错误定位手册

出问题先看状态,不要靠猜。

### 13.1 `ls /` 看不到 mcu2

1. `cat /mcu1/sys/ports`：mcu1 是否看到 mcu2 端口。
2. `cat /mcu1/sys/links`：link 是否双向。
3. `cat /mcu2/sys/health`：mcu2 是否拿到 addr。
4. host `/host/sys/topology`：host 是否收到 mcu1 上报的 link state。
5. host `/host/sys/routes`：是否给 mcu2 装了 route。

不要直接改 `ls`。

### 13.2 `cat /mcu1/sys/health` 在 mcu2 上电后失败（P0）

1. mcu1 task heartbeat 是否停（`/sys/tasks`）。
2. mcu1 upstream 端口是否变 SUSPECT（`/sys/ports`）。
3. mcu1 的 mcu2 端口是否抢占了全局锁。
4. 控制面是否误删 mcu1 route。
5. session_manager 是否把 mcu2 错误广播到 mcu1 session。

架构要求：此问题必须被隔离在 mcu2 端口或 mcu2 session。

### 13.3 立即返回 EAGAIN

通常是错误映射问题。正确行为：no route→`E_NO_ROUTE`、queue full→`E_QUEUE_FULL`、link down→`E_LINK_DOWN`、等到 deadline→`E_DEADLINE`(上层可显示 EAGAIN)。

查：请求是否真进了 pending 表、deadline 是否被设成 0 或已过期、是否 no route 被错映射成 EAGAIN、是否 queue full 被映射成 EAGAIN。

### 13.4 反复 REGISTER

1. 节点是否真重启：看 boot_id。
2. lease 是否过短或续约失败。
3. host 是否没保存 uid→node name 映射。
4. port_manager 是否不断把 upstream 设 down/up。
5. host 是否把重复 register 当新节点。

重复 REGISTER 不一定是错;重复建节点、重复 attach、重复清 session 才是错。

---

## 14. 目标目录结构

```text
9P-Walkers/
├── docs/
│   ├── refactor_plan.md              # 本文件（唯一重构基线）
│   ├── architecture.md               # 现状架构（重构推进时同步更新）
│   ├── adr/                          # 架构决策记录
│   └── logs/refactor/                # 各里程碑验收记录
│
├── pwos-shared/                      # 主从共享
│   ├── link/                         # 【新】统一 link frame/parser/crc + PC 测
│   ├── mesh2/                        # 【新】mesh 控制面 v2: identity/lease/topology/route
│   ├── mini9p/                       # 【保留】协议/client/server
│   └── codec/                        # 【新】CBOR 编解码（host_rpc + RPC + job 共用）
│
├── pwos-slave/                       # STM32F407 从机（唯一从机工程）
│   ├── Core/                         # CubeMX: 时钟/GPIO/UART/DMA/FreeRTOS
│   └── User/
│       ├── board/                    # 【新】board_ports 候选 UART 列表（不写死上下游）
│       ├── rtos/                     # 【新】任务与队列装配
│       ├── os/                       # 【新】frame_pool 零拷贝块池
│       ├── drivers/                  # uart_dma_port + 外设驱动
│       ├── link/                     # 【新】port_manager + frame_parser 接入
│       ├── mesh2/                    # 【新】node_control + forwarder
│       ├── service/                  # 【新】mini9P server + RPC server + job worker（service 任务）
│       └── resources/                # sys/dev/fs/compute 后端
│
├── pwos-master-esp32p4/              # ESP32-P4 主控
│   ├── main/                         # app_main + 任务装配
│   ├── host_coordinator/            # 【新】coordinator/topology_db/route_planner/lease
│   ├── host_sessions/               # 【新】session_manager
│   ├── host_api/                    # cluster_vfs / rpc_client / job_manager
│   ├── host_rpc/                    # 【新】主机间 TCP+mDNS+CBOR
│   ├── host_link/                   # uart_link / tcp_link / link_mux
│   ├── shell/  web/  lua/           # 上层入口
│   └── (build/ managed_components/ 不入库)
│
└── tools/pc_master_emulator/         # PC 串口联调工具（保留并随协议更新）
```

迁移期允许 `link`/`mesh2` 与旧 `mesh` 并存,等新栈在硬件稳定后再删除旧模块。

---

## 15. 术语表

| 术语 | 定义 |
|---|---|
| **节点 UID** | 物理 MCU 的稳定硬件身份,重启/重连/重命名不变 |
| **boot_id** | 每次启动变化的随机数,用来识别"是否真的重启" |
| **节点名** | 主机分配的用户可见名,如 `mcu1`,作为命名空间第一级 |
| **节点地址 / addr** | mesh envelope 的短地址,coordinator 租约分配,可回收,不进用户路径 |
| **lease / 地址租约** | 节点地址的有期限授予,需续约,有 epoch 版本 |
| **mesh envelope** | 有线骨干的帧封装,承载控制面 + mini9P + 计算数据 |
| **有线 mesh 骨干平面** | 主机↔STM32↔STM32 的 UART 网络,STM32 中转转发 |
| **主机间 IP 平面** | ESP32 主机间经路由器的 TCP 网络,跑 host_rpc,STM32 不可见 |
| **host_rpc** | 主机间 RPC 协议(TCP+mDNS+CBOR),独立于 mesh envelope |
| **控制面** | 注册/命名/拓扑/路由/lease/心跳,优先级高于数据面 |
| **数据面** | mini9P 文件访问 / RPC / 计算数据 |
| **端口角色** | 从机每个 UART 的动态角色(UPSTREAM/DOWNSTREAM/PEER/QUARANTINED…),协议发现而非静态配置 |
| **全局拓扑图** | 主机维护的集群连接图,派生路由 |
| **本地转发表** | 从机的简化投递规则(dst→next_hop),由主机下发 |
| **frame_pool** | 从机零拷贝固定块池,块=526B,队列传指针不传帧体 |
| **pending 表** | 主机在途请求表,按 (mesh_addr, tag) 匹配响应 |
| **leader / follower / observer** | 多主角色:分配地址路由 / 同步可接管 / 只读 |

---

> **下一步**：从 M0 开始。先把 baseline 冻结、F411 与 build 产物清理、四个根因实测复现记录进 `docs/logs/refactor/M0-baseline.md`,再进 M1 写 `pwos-shared/link/`。每完成一个里程碑,在 `docs/logs/refactor/` 留验收记录。
