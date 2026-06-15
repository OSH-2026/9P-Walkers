# 9P-Walkers 重构架构蓝图

本文档描述下一版系统的目标架构。它不是当前代码说明，也不是修 bug 记录，而是重构时用来约束模块边界、任务模型和协议演进的基线。

## 1. 业务目标

9P-Walkers 要做的是一个 MCU 集群操作系统原型：

- 主控或多个主机通过统一命名空间访问一组 MCU 节点。
- 每个 MCU 节点把本地状态、文件、外设、计算能力暴露为可寻址资源。
- 网络可以由 UART、LAN/WiFi、未来 SPI/CAN/USB 等链路组成。
- 节点之间可以中继，主机不必直连所有从机。
- 上层入口包括 WebShell、串口 shell、Lua 脚本、RPC 客户端和自动调度器。
- 后续要支持 RPC、多主机、分布式计算、任务迁移或任务下发。

核心用户视角仍然应该足够简单：

```text
cat /mcu1/sys/health
cat /mcu2/sensor/temp
rpc /mcu3.compute.matmul '{"a": "...", "b": "..."}'
submit --target any-gpu-like-node job.json
```

## 2. 当前主要问题

这些问题不是某个单点 bug，而是架构边界不清导致的系统性风险：

- 从机 mesh、Mini9P、业务 VFS、UART 轮询都挤在一个主循环里，任何下游口异常都可能拖死本机业务。
- 串口资源和端口角色是静态假设，代码里隐含“某个端口是上游、某个端口是下游”，硬件拓扑一变就失效。
- 控制面和数据面互相穿透，REGISTER、LINK_STATE、ROUTE_UPDATE、Mini9P 请求会在等待响应时相互影响。
- 路由状态、VFS attach 状态、Mini9P client session 状态分散在多个模块，容易出现“看起来在线但实际不可用”。
- transport 层混入 frame queue、健康判断、重启策略等策略逻辑，难以定位问题。
- 多数模块缺少明确所有权：谁拥有地址，谁拥有链路状态，谁能删除路由，谁能重置会话，没有统一规则。
- 代码可读性不足，注释没有解释设计意图，后续维护只能靠猜。

重构的目标不是把所有代码重写成更多代码，而是把状态机、任务边界和模块所有权写清楚，然后逐步替换。

## 3. 架构原则

1. 单一所有权：每类状态只能由一个模块拥有，其他模块通过消息或只读查询访问。
2. 控制面优先：发现、心跳、地址租约、路由更新不能被普通文件读写阻塞。
3. 数据面隔离：Mini9P/RPC 请求不能直接驱动链路发现、端口重启或全局路由删除。
4. 从机必须自治：从机自己管理 UART 端口、邻居关系、主机候选、端口健康，不依赖固定拓扑。
5. RTOS 优先：从机从一开始就按 FreeRTOS 任务拆分，禁止把 mesh 和主业务放在同一个无限循环里。
6. 链路不可信：任何端口都可能噪声、断电、半连接、反复重启；异常端口必须被隔离，不能拖垮其他端口。
7. 协议可演进：frame type、capability、lease、epoch 必须留扩展空间，支持多主机和未来 RPC。
8. 可观测性内建：每个节点必须能输出端口、任务、队列、路由、会话、错误计数，而不是出问题后临时加日志。
9. 测试先于硬件集成：状态机和路由算法必须有 PC 单测，硬件只验证驱动和电气问题。

## 4. 目标总体架构

```text
Host UI / API
  WebShell / Serial Shell / Lua / HTTP RPC / Scheduler
        |
        v
Host Resource API
  Cluster VFS + RPC Client + Job Manager
        |
        v
Host Mesh Core
  Coordinator / Topology DB / Route Planner / Session Manager
        |
        v
Link Manager
  UART / TCP / future SPI-CAN-USB transports
        |
        v
Mesh Network
        |
        v
Node Link Manager
  per-port drivers, role detection, health, queues
        |
        v
Node Mesh Core
  identity, address lease, neighbor table, route table, forwarding
        |
        v
Node Service Runtime
  Mini9P server, RPC server, job executor
        |
        v
Local Resources
  sys, dev, fs, sensors, actuators, compute workers
```

## 5. 分层职责

### 5.1 Transport Driver

位置建议：

```text
pwos-shared/link/
pwos-slave/User/link/
pwos-master-esp32p4/link/
```

职责：

- 只负责字节流收发、DMA、硬件错误恢复、TX 完成等待。
- 不理解 mesh 地址、REGISTER、Mini9P、路由。
- 每个物理端口有独立 RX ring、TX queue、统计计数。
- ISR 只搬运数据或发通知，不解析协议，不做路由，不调用 VFS。

必须提供的接口：

```c
int link_port_open(port_id, config);
int link_port_write(port_id, bytes, len, deadline);
int link_port_read_bytes(port_id, out, cap);
int link_port_get_stats(port_id, *stats);
int link_port_set_enabled(port_id, bool enabled);
```

### 5.2 Link Framer

职责：

- 从字节流中切出完整 mesh frame。
- 做 magic、长度、CRC 校验。
- 把完整帧投递给 `mesh_ingress_queue`。
- 坏帧只影响当前端口，不影响全局 mesh。

关键规则：

- 每个端口独立 parser。
- 坏帧超过阈值，端口进入 quarantine。
- quarantine 期间只丢弃/限速该端口输入，上游端口不受影响。

### 5.3 Port Manager

这是从机重构的关键模块。

职责：

- 根据板级候选 UART 列表初始化端口池。
- 动态探测端口是否有邻居。
- 维护端口角色：
  - `UNKNOWN`
  - `HOST_CANDIDATE`
  - `UPSTREAM`
  - `DOWNSTREAM`
  - `PEER`
  - `DISABLED`
  - `QUARANTINED`
- 维护端口健康：
  - rx bytes
  - valid frames
  - bad frames
  - overruns
  - dropped frames
  - last hello time
  - last heartbeat time
- 为 mesh core 提供端口选择能力，但不决定全局路由。

端口发现流程：

```text
BOOT
  -> enumerate board candidate UARTs
  -> start DMA RX for each enabled port
  -> periodically send LINK_HELLO on each candidate port
  -> receive LINK_HELLO_ACK / HOST_ADVERTISE / NODE_ADVERTISE
  -> classify port role
```

从机不能假设 USART1/USART2/USART3 哪个是主机。它只能根据协议发现：

- 对端声明 `ROLE_COORDINATOR`：该端口是 host candidate。
- 对端声明 `ROLE_NODE` 或 `ROLE_ROUTER`：该端口是 neighbor/downstream candidate。
- 同时看到多个 host candidate：交给 Coordinator Client 做主机选择。

### 5.4 Mesh Core

职责：

- 拥有本节点 mesh identity、地址租约、邻居表、路由表。
- 处理 mesh 控制面：
  - `LINK_HELLO`
  - `NODE_REGISTER`
  - `ADDR_ASSIGN`
  - `HEARTBEAT`
  - `LINK_STATE`
  - `ROUTE_UPDATE`
  - `HOST_ADVERTISE`
  - `ERROR`
- 转发非本机数据面 frame。
- 向 service runtime 投递本机数据面请求。

Mesh Core 不允许：

- 直接读写 UART 寄存器。
- 直接访问本地文件系统或外设。
- 在等待某个 RPC/Mini9P 响应时阻塞控制面。

### 5.5 Coordinator Client

每个从机都应该有 coordinator client，而不是写死“主机就是上游端口”。

职责：

- 从 host candidates 中选择当前主机。
- 维护地址 lease：
  - `UNASSIGNED`
  - `ASSIGNED`
  - `RENEWING`
  - `EXPIRED`
- 处理多主机：
  - 读取 host priority、epoch、cluster id。
  - 选择 leader host。
  - 备用 host 只作为 failover，不同时下发冲突路由。
- lease 到期前续租。
- 主机切换时通知 Mesh Core 进入 route reconverge。

主机选择规则建议：

```text
优先级高者胜出
同优先级 epoch 新者胜出
同 epoch host_uid 小者胜出
当前 host 仍健康时避免频繁切换
```

### 5.6 Host Coordinator

主机侧拆成 Coordinator，而不是把所有逻辑塞进 `mesh_host_runtime`。

职责：

- 维护全局 topology DB。
- 为节点分配地址 lease。
- 根据链路状态计算路由。
- 下发 route update。
- 处理多主机协调：
  - leader election
  - topology replication
  - read-only observer host
  - failover
- 向 Cluster VFS / RPC Scheduler 提供稳定节点视图。

Host Coordinator 拥有：

- address allocator
- topology database
- route planner
- node registry
- lease table
- host epoch

Host Coordinator 不拥有：

- WebShell UI。
- Mini9P fid 表。
- 具体 UART DMA。
- 从机业务状态。

### 5.7 Session Manager

职责：

- 管理 Mini9P attach/session 状态。
- 管理 RPC stream/request 状态。
- 把 transport/route 错误映射成上层错误。
- 支持重试、超时、取消。

关键点：

- VFS route online 不等于 Mini9P attached。
- route reachable 不等于 RPC session ready。
- session reset 不能直接删除节点或链路。

### 5.8 Resource Layer

从机本地资源统一挂到资源注册表。

资源类型：

- `sys`: health, info, logs, task stats, route stats。
- `dev`: GPIO, motor, sensor, UART bridge 等外设。
- `fs`: littlefs 文件。
- `rpc`: 函数式调用。
- `compute`: 长任务、批处理、分布式计算 worker。

建议目录：

```text
/sys/health
/sys/tasks
/sys/ports
/sys/routes
/sys/sessions
/dev/...
/fs/...
/rpc/<service>/<method>
/compute/jobs
/compute/workers
```

Mini9P 可以继续作为文件语义入口；RPC 不要硬塞进 Mini9P 文件读写里，而应该作为独立数据面类型，文件系统只提供调试和兼容入口。

## 6. 从机 FreeRTOS 任务模型

从机必须从一开始使用 RTOS。推荐任务如下。

| 任务 | 优先级 | 职责 | 禁止事项 |
| --- | --- | --- | --- |
| `link_rx_task[N]` | 高 | 每个端口收字节、切帧、投递 ingress queue | 不解析业务、不访问 VFS |
| `link_tx_task` | 高 | 串行化每端口发送，处理 TX 超时 | 不等待业务结果 |
| `mesh_ctrl_task` | 高 | 处理 REGISTER、ASSIGN、HELLO、HEARTBEAT、ROUTE_UPDATE | 不做长计算 |
| `mesh_router_task` | 中高 | 转发非本机 frame，做 hop/TTL 检查 | 不访问文件系统 |
| `session_task` | 中 | Mini9P/RPC session 状态机 | 不直接操作硬件 |
| `resource_task` | 中 | 本机 VFS/RPC 请求分发 | 不处理 mesh 控制面 |
| `app_task` | 中低 | 用户主业务、传感器、控制逻辑 | 不阻塞 mesh |
| `compute_worker_task[N]` | 低 | 长计算任务 | 不占用高优先级锁 |
| `diag_task` | 低 | 统计、日志、watchdog feed | 不改变路由 |

核心队列：

```text
link_rx_task[N] -> mesh_ingress_queue
mesh_ctrl_task  -> route_event_queue
mesh_router_task -> link_tx_queue
session_task -> resource_request_queue
resource_task -> resource_response_queue
compute_worker -> job_event_queue
```

硬约束：

- 任意 task 不能在持有全局 mesh lock 时做文件 IO。
- 任意 task 不能在 ISR 中解析完整协议。
- 任意 task 不能无限等待另一个 task，必须有 deadline。
- 控制面队列满时，优先丢弃低价值重复心跳，不丢地址租约和路由撤销。

## 7. 从机端口自分配设计

板级代码只提供候选端口描述，不指定上下游：

```c
static const board_port_desc ports[] = {
    { .id = 0, .uart = &huart1, .name = "USART1" },
    { .id = 1, .uart = &huart2, .name = "USART2" },
    { .id = 2, .uart = &huart3, .name = "USART3" },
    { .id = 3, .uart = &huart4, .name = "UART4"  },
    { .id = 4, .uart = &huart6, .name = "USART6" },
};
```

启动流程：

```text
1. Port Manager 打开所有候选端口。
2. 每个端口周期发送 LINK_HELLO(uid, local_caps, boot_nonce, port_id)。
3. 收到对端 HELLO 后建立 link-local neighbor。
4. 如果对端 caps 含 ROLE_COORDINATOR，则加入 host candidates。
5. Coordinator Client 选择主机，向其发送 NODE_REGISTER。
6. 收到 ADDR_ASSIGN 后进入 ASSIGNED。
7. Mesh Core 向所有邻居上报 LINK_STATE。
8. Route Update 收敛后数据面开始稳定转发。
```

端口角色不是永久的。断线、心跳超时、错误过多都可以触发降级：

```text
UPSTREAM -> HOST_CANDIDATE -> UNKNOWN -> QUARANTINED -> UNKNOWN
DOWNSTREAM -> UNKNOWN
PEER -> UNKNOWN
```

## 8. 邻居和主机识别

不要再通过端口位置判断主机或邻居。必须通过协议 capability 判断。

建议 capability：

```text
ROLE_COORDINATOR     主机/地址分配者
ROLE_NODE            普通节点
ROLE_ROUTER          可转发
ROLE_STORAGE         有文件存储
ROLE_RPC             支持 RPC
ROLE_COMPUTE         支持计算任务
ROLE_TIME_SOURCE     可提供时间同步
LINK_UART            当前链路是 UART
LINK_TCP             当前链路是 TCP
```

判断规则：

- `ROLE_COORDINATOR` 是主机候选，不一定是当前主机。
- 当前主机由 Coordinator Client 根据 priority/epoch/health 选出。
- `ROLE_ROUTER` 是可中继邻居。
- 普通节点也可以是下游邻居，但不能承担全局地址分配。

## 9. 协议重构建议

当前 mesh envelope 可以保留，但控制面类型建议升级。

### 9.1 链路本地控制帧

链路本地帧不需要全局 mesh 地址，适合未分配地址阶段：

```text
LINK_HELLO
LINK_HELLO_ACK
LINK_HEARTBEAT
LINK_ERROR
```

字段：

```text
version
node_uid
boot_nonce
port_local_id
capability_bits
role_bits
link_type
mtu
protocol_flags
```

### 9.2 全局控制帧

全局控制帧需要 mesh 地址和 coordinator epoch：

```text
NODE_REGISTER
ADDR_ASSIGN
LEASE_RENEW
LEASE_ACK
LINK_STATE
ROUTE_UPDATE
HOST_ADVERTISE
HOST_SYNC
TIME_SYNC
```

关键字段：

```text
cluster_id
host_uid
host_epoch
lease_id
route_version
seq
ttl
```

### 9.3 数据面帧

数据面类型：

```text
DATA_MINI9P
DATA_RPC
DATA_STREAM
DATA_JOB
DATA_BULK
```

Mini9P 负责文件语义；RPC 负责方法调用；JOB/BULK 负责长任务和大数据。

## 10. RPC 设计

RPC 不应该伪装成普通文件读写。建议单独定义 `DATA_RPC`。

RPC 请求：

```text
rpc_id
service_id
method_id
content_type
deadline_ms
flags
payload
```

RPC 响应：

```text
rpc_id
status
error_code
payload
```

支持模式：

- unary request/response。
- streaming request/response。
- fire-and-forget command。
- cancel。
- deadline。

RPC 服务注册：

```text
/rpc/services
/rpc/<service>/methods
```

调度器可以基于 capability 选择节点：

```text
ROLE_COMPUTE + service=matmul + memory>=X + load<Y
```

## 11. 分布式计算设计

分布式计算不要直接跑在 shell 里，应有 Job Manager。

Job Manager 职责：

- 接收任务描述。
- 拆分子任务。
- 根据节点 capability 和负载调度。
- 追踪状态：
  - queued
  - assigned
  - running
  - partial_done
  - failed
  - cancelled
  - done
- 收集结果。
- 支持重试和迁移。

节点 Compute Worker 职责：

- 暴露可执行 kernel 列表。
- 接收 job chunk。
- 上报进度和资源占用。
- 支持取消。
- 不阻塞 mesh 控制面。

建议接口：

```text
/compute/caps
/compute/load
/compute/jobs/<job_id>/status
/compute/jobs/<job_id>/log
RPC compute.submit
RPC compute.cancel
RPC compute.get_result
```

## 12. 多主机设计

多主机是未来高级功能，但底层字段现在就要预留。

主机角色：

- `LEADER`: 地址分配、路由规划、写 topology。
- `FOLLOWER`: 同步 topology，可接管。
- `OBSERVER`: 只读监控，可发读请求但不改路由。
- `CLIENT`: 上层 API 入口，不参与控制面。

核心机制：

- 每个 host 有稳定 `host_uid`。
- 每个 cluster 有 `cluster_id`。
- leader 维护 `host_epoch`。
- 节点只接受当前 leader 的地址租约和 route update。
- follower 可接收 host sync，leader 超时后发起选举。

冲突处理：

- epoch 小的 host 命令被拒绝。
- 同 epoch 多 leader 时，host_uid 小者或 priority 高者胜出。
- 节点记录最近接受的 coordinator，避免来回抖动。

## 13. 主机侧模块重构

建议拆分：

```text
host_coordinator/
  coordinator.c
  address_allocator.c
  topology_db.c
  route_planner.c
  lease_table.c

host_sessions/
  mini9p_session_manager.c
  rpc_session_manager.c

host_api/
  cluster_vfs.c
  rpc_client.c
  job_manager.c

host_link/
  uart_link.c
  tcp_link.c
  link_mux.c
```

主机控制面流程：

```text
link_rx -> frame_decode -> coordinator_event_queue
coordinator -> topology_db / lease_table / route_planner
route_planner -> route_update_queue
route_update_queue -> link_tx
```

主机数据面流程：

```text
shell/web/lua -> vfs/rpc/job API
API -> session_manager
session_manager -> mesh_data_queue
mesh_data_queue -> route lookup -> link_tx
link_rx -> data_response_queue -> session_manager -> API
```

## 14. 从机侧模块重构

建议拆分：

```text
node_link/
  board_ports.c
  port_manager.c
  uart_dma_port.c
  frame_parser.c

node_mesh/
  identity.c
  coordinator_client.c
  neighbor_table.c
  route_table.c
  forwarder.c
  mesh_control_task.c

node_services/
  mini9p_service.c
  rpc_service.c
  resource_registry.c
  job_worker.c

node_resources/
  sys_resource.c
  dev_resource.c
  fs_resource.c
  compute_resource.c
```

从机启动：

```text
board_init
  -> rtos_start
  -> port_manager_start
  -> mesh_core_start
  -> service_runtime_start
  -> app_start
```

## 15. 可观测性要求

每个节点必须提供：

```text
/sys/health
/sys/tasks
/sys/ports
/sys/links
/sys/neighbors
/sys/routes
/sys/sessions
/sys/queues
/sys/log
```

每个端口至少输出：

```text
port_id
role
state
rx_bytes
tx_bytes
valid_frames
bad_frames
dropped_frames
overrun_count
quarantine_until
last_rx_ms
last_valid_frame_ms
peer_uid
peer_role
```

每个 task 至少输出：

```text
name
priority
stack_high_watermark
queue_depth
last_run_ms
error_count
```

## 16. 测试策略

必须新增以下测试层级：

1. 协议编解码单测。
2. 链路 parser 单测：随机噪声、半帧、CRC 错、连续帧。
3. Port Manager 状态机单测。
4. Coordinator Client 主机选择单测。
5. Host Coordinator 地址租约和路由规划单测。
6. Mini9P session reset/reattach 单测。
7. RPC deadline/cancel 单测。
8. Job Manager 调度单测。
9. 硬件 smoke test：
   - 单主机单节点。
   - 单主机链式两节点。
   - 下游节点反复上电不影响上游节点本机资源。
   - 下游噪声口隔离。
   - 主机断开后重连。

每个修复都必须有至少一个复现测试或诊断项。

## 17. 迁移路线

### Phase 0: 冻结补丁式改动

- 当前分支只允许加诊断和文档。
- 不再继续往现有主循环里塞复杂逻辑。
- 明确保留哪些代码可复用：
  - Mini9P protocol。
  - mesh envelope 编解码。
  - local VFS backend。
  - WebShell 基础 UI。

### Phase 1: 建立新链路层

- 新建 `link` 模块。
- STM32 UART 改成 per-port DMA + ring buffer + parser。
- 不接入现有 mesh runtime，先做 PC 和硬件 loopback 测试。
- 完成端口噪声隔离测试。

### Phase 2: 建立从机 RTOS 骨架

- CubeMX 启用 FreeRTOS。
- 建立任务和队列，但业务先用 mock。
- 把 mesh 收发从主循环移到 `mesh_ctrl_task` / `mesh_router_task`。
- 保证 `/sys/tasks`、`/sys/ports` 可读。

### Phase 3: 重写从机 discovery

- 实现 LINK_HELLO。
- 从机动态识别 host/neighbor。
- 去掉固定 upstream port 假设。
- 实现地址 lease renew。

### Phase 4: 重写 host coordinator

- 拆出 topology DB、address allocator、route planner。
- 保留现有 Cluster VFS API，但后端改接新 coordinator。
- 完成链式拓扑稳定测试。

### Phase 5: 接入 Mini9P 数据面

- Mini9P server/client 复用现有协议。
- Session Manager 负责 attach/reattach。
- VFS 不直接关心 mesh route 细节。

### Phase 6: 接入 RPC 和 compute

- 新增 DATA_RPC。
- 从机 resource registry 支持 RPC service。
- Job Manager 支持长任务。
- 完成一个最小分布式计算 demo。

### Phase 7: 多主机

- host advertise。
- leader/follower/observer。
- topology sync。
- failover 测试。

## 18. 代码质量要求

每个模块合并前必须满足：

- README 或模块注释说明职责、非职责、状态机。
- 公开函数注释说明输入、输出、错误码、线程上下文。
- 状态机必须有图或表。
- 不允许没有测试的协议变更。
- 不允许在 ISR 中调用业务逻辑。
- 不允许跨层直接访问内部结构体。
- 不允许“临时修一下”的全局变量绕过模块 API。
- PR 描述必须包含：
  - 改了哪个模块。
  - 这个模块拥有什么状态。
  - 这个改动影响哪些任务/队列。
  - 如何测试。

## 19. 立即决策项

重构开始前需要负责人确认：

1. 从机 RTOS 采用 CMSIS-RTOS2 API 还是直接 FreeRTOS API。
2. 新协议是否允许破坏当前 mesh frame 兼容性，还是保留 v1/v2 双栈。
3. 多主机第一版是否只做 `LEADER + OBSERVER`，暂不做自动 failover。
4. RPC payload 使用 CBOR、MessagePack、JSON 还是自定义 TLV。
5. 分布式计算第一版 demo 选什么任务：矩阵乘、Mandelbrot、模型推理切片或传感器融合。

建议选择：

- 从机直接用 FreeRTOS API，少一层 CMSIS 抽象。
- mesh envelope 保留 version 字段，v1 当前协议只维护，v2 做新控制面。
- 多主机第一版做 `LEADER + OBSERVER`。
- RPC payload 第一版用 CBOR 或 TLV，不建议 JSON 跑在 MCU 数据面。
- 分布式计算 demo 选 Mandelbrot 或矩阵乘，便于验证分片和结果合并。

## 20. 最小可交付目标

第一轮重构不要追求完整高级功能。最小可交付定义为：

```text
ESP32 host -- UART -- mcu1 -- UART -- mcu2
```

必须稳定满足：

- mcu1/mcu2 自己发现端口角色。
- mcu2 反复上电不影响 `cat /mcu1/sys/health`。
- host 能访问 `/mcu1/sys/*` 和 `/mcu2/sys/*`。
- 从机主业务 task 独立运行，不因 mesh 卡死。
- `/sys/ports` 能看出端口健康和隔离状态。
- 所有控制面状态机有 PC 单测。

这个目标达成后，再接 RPC、多主机和分布式计算。
