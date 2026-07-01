# 9P-Walkers 当前架构

## 1. 系统边界

系统包含两个相互独立、只在 ESP32 主机汇合的网络平面。

```text
┌──────────────── 主机间 IP 平面 ────────────────┐
│ ESP32-P4 Ethernet <-> router <-> ESP32-S3 WiFi │
│ mDNS discovery + TCP/9909 + CBOR host RPC      │
└───────────────┬───────────────────┬────────────┘
                │ UART              │ UART
┌───────────────▼───────────────────▼────────────┐
│ STM32 有线 mesh 平面                           │
│ link frame v2 + mesh2 control + typed data     │
│ 节点可以作为 relay，多 UART、多跳转发           │
└────────────────────────────────────────────────┘
```

主机间不透传 STM32 link frame。跨主机访问由 owner 主机终止 host RPC，再在自己的
STM32 子树中发起 mini9P/RPC/Job 请求。

## 2. STM32 平面

### 2.1 分层

```text
HAL UART/DMA
  -> uart_dma_port
  -> frame_pool + FreeRTOS queues
  -> port_manager
  -> node_control
  -> service_runtime
       -> mini9P server -> local_vfs
       -> rpc_service
       -> job_service -> compute_worker
```

- `uart_dma_port` 拥有 DMA、parser、TX 完成和硬件错误统计。
- `port_manager` 拥有每个物理端口的 HELLO FSM 和 peer 身份。
- `node_control` 拥有本机地址、lease、upstream、route 和 relay pending。
- `service_runtime` 是本机数据面唯一入口，不参与链路发现和路由决策。
- `compute_worker` 在低优先级任务中增量执行 kernel，避免阻塞控制面。

ISR 只搬运字节、更新 DMA 状态并通知任务，不执行 mesh、mini9P、RPC 或计算逻辑。

### 2.2 控制面

1. 相邻端口交换 `LINK_HELLO/ACK`。
2. 未分配节点选择 coordinator 或可到达 upstream 的 relay。
3. `CTRL_NODE_REGISTER` 可由已分配 relay 向上游转发。
4. coordinator 返回 `CTRL_ADDR_ASSIGN`，节点进入 lease 周期。
5. 节点上报 `CTRL_LINK_STATE`，主机计算并下发 `CTRL_ROUTE_UPDATE`。
6. 数据帧按目标地址本地投递或递减 TTL 后转发。

## 3. 主机内部

```text
UART RX task (唯一消费者)
  -> coordinator_runtime
       -> host_coordinator       地址、lease、拓扑、路由
       -> session_manager        typed pending、tag、deadline
       -> cluster_vfs            /mcuN 路径、fid、generation
       -> slave_rpc              DATA_RPC client
       -> job_manager            DATA_JOB 生命周期

WebShell / Lua / inference
  -> host_shell / host_api / host_jobs
  -> 上述主机服务
```

P4 额外运行 Ethernet、HTTP/WebSocket、Lua 渲染调度器。S3 运行 WiFi STA 和推理；
两者复用 coordinator、session、job 和 host RPC 实现。

## 4. 主机间平面

- P4 从 Ethernet MAC、S3 从 WiFi STA MAC 派生稳定 `host_uid` 和 hostname。
- `_pwos._tcp` mDNS 服务发布 TCP/9909。
- `epoch` 持久化在 NVS；主机按 `(epoch, priority, host_uid)` 选举 leader。
- leader 汇总 owner 节点快照并分配全局 `mcuN` 名称。
- follower 保留 `global_target -> owner_target` 映射。
- 当前跨主机支持节点 read/write；远端目录 list/stat 和高吞吐 bulk 不在 alpha 范围。

## 5. 数据面

| 类型 | 用途 | 主机端 | STM32 端 |
|---|---|---|---|
| `DATA_MINI9P` | 文件/诊断 | `cluster_vfs` + `session_manager` | `mini9p_server` + `local_vfs` |
| `DATA_RPC` | 短调用/流 | `slave_rpc` | `rpc_service` |
| `DATA_JOB` | 异步计算 | `job_manager` | `job_service` + `compute_worker` |
| `DATA_BULK` | 大数据预留 | 未实现 | 未实现 |

pending 键至少包含 `(data_type, src_addr, tag)`。节点 boot ID 改变时，主机清理该节点
旧 session、fid 和未完成事务，避免旧响应污染新实例。

## 6. 分布式计算

Job System 使用静态槽和有界 payload。当前 kernel 包括 hash、vector add、matmul、
Mandelbrot 和 raytrace tile。P4 的 Lua 调度器动态枚举在线 MCU，将 240x320 场景拆成
16x7 tile，完成后写入 F429 的 `/display/tile`。

P4/S3 的 LLM 引擎独立于 MCU Job System；`dist_inference_service` 通过 host RPC 提供
主机间推理协作接口，目前仍属于实验能力。

## 7. 关键约束

1. UID 是物理节点身份；短地址和 `mcuN` 名称都可重新分配。
2. coordinator 拥有全局拓扑；STM32 不运行全局路由算法。
3. link、控制面和数据面状态分别由单一模块拥有。
4. 热路径不动态分配内存；协议、队列和 job 使用固定上限。
5. UART RX 每个平台只有一个消费者；上层通过 pending/semaphore 等待结果。
6. 控制面不能因文件访问、RPC、Job 或渲染而阻塞。
7. 主机间平面只用于可信局域网，当前没有 TLS 或鉴权。

## 8. 代码位置

```text
pwos-shared/
  link/ mesh2/ mini9p/ rpc/ job/ host_rpc/ render/

pwos-master-esp32p4/
  coordinator_runtime/ host_coordinator/ host_sessions/
  host_api/ slave_rpc/ host_jobs/ host_rpc/
  host_shell/ host_net/ web/ render/ inference/

pwos-master-esp32s3/
  main/ host_net/ inference/

pwos-slave*/User/
  drivers/ link/ mesh2/ rtos/ os/ service/ compute/ backend/ diag/
```
