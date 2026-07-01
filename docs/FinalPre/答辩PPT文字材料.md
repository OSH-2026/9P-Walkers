# 9P-Walkers 答辩 PPT 文字材料

> 本材料基于仓库源码、配置文件、文档和测试脚本全面分析生成，技术细节均来自真实代码，文件路径已标注。可直接改写成 40 分钟答辩 PPT。

---

## 1. 仓库探索总结

**项目类型**：面向异构 MCU 集群的实验性分布式操作系统/协议栈。

**技术栈**：
- 主机：ESP32-P4（Ethernet）、ESP32-S3（WiFi），ESP-IDF 6.0 + FreeRTOS。
- 节点：STM32F407/STM32F429，HAL + FreeRTOS（CMSIS-RTOS V2）。
- 协议：自定义 link frame v2、mesh2 控制面、mini9P 远端文件协议、MCU RPC v1、Job v1、Host RPC v1（TCP/9909 + CBOR）。
- 推理：llama.cpp + Ray（Lab4）。

**入口文件**：
- ESP32-P4：`pwos-master-esp32p4/main/app_main.c`
- STM32F407：`pwos-slave/Core/Src/main.c` → `MX_FREERTOS_Init()` → `pwos_tasks_start()`
- STM32F429：`pwos-slave-stm32f429/Core/Src/main.c`

**构建方式**：
- STM32：CMake + Ninja + GNU Arm Embedded Toolchain，OpenOCD 烧录。
- ESP32：`idf.py build/flash/monitor`。
- 测试：各模块独立 CMake 工程，构建到 `/tmp`。

**核心模块**：
- `pwos-shared/`：link、mesh2、mini9P、RPC、Job、host_rpc、render。
- `pwos-master-esp32p4/`：coordinator_runtime、host_coordinator、host_sessions、cluster_vfs、slave_rpc、job_manager、host_rpc、web、Lua、inference。
- `pwos-slave*/User/`：uart_dma_port、port_manager、node_control、service_runtime、local_vfs、rpc_service、job_service、compute_worker。

**已验证的阅读深度**：已直接阅读上述所有核心模块的源码与头文件，以及 Lab4 完整实验报告。

---

## 2. 项目整体理解

### 2.1 一句话概括

9P-Walkers 是一个**异构 MCU 集群实验系统**：ESP32 主机通过 UART 管理 STM32 节点组成多跳有线 mesh，主机间通过 TCP/CBOR 互联，向上暴露统一的 `/mcuN/...` 命名空间，支持远端文件、RPC、异步 Job 和分布式渲染/推理。

### 2.2 项目背景与应用场景

**背景**：
- 传统物联网节点各自独立，缺乏统一的命名、通信和计算调度能力。
- 需要在资源受限设备上实现可扩展的集群计算、远程诊断和任务分发。

**应用场景**：
- 边缘计算：把轻量计算任务（hash、矩阵乘法、Mandelbrot、光线追踪 tile）分发到多个 STM32。
- 分布式诊断：通过 `/mcuN/sys/health` 等统一路径读取节点状态。
- 多主机协作：P4 有线上网 + S3 WiFi，组成局域网集群，全局命名 `mcuN`。
- 分布式推理：Lab4 验证 llama.cpp RPC 和 Ray 批量调度。

### 2.3 项目要解决的核心问题

1. **异构设备如何统一命名与访问？** → `/mcuN/...` cluster VFS + mini9P。
2. **多跳拓扑如何自动发现与路由？** → HELLO/ACK + 注册/租约/链路状态/路由下发。
3. **控制面与数据面如何隔离不阻塞？** → 独立任务、队列、帧池、typed pending。
4. **主机间如何协作？** → mDNS + TCP/9909 + CBOR host RPC + leader election。
5. **长计算如何不阻塞通信？** → 低优先级 compute_task 增量执行 kernel。

---

## 3. 项目架构图的文字描述

```
┌─────────────────────────────────────────────────────────────┐
│                    主机间 IP 平面                            │
│   ESP32-P4 (Ethernet)  <--mDNS+TCP/9909+CBOR-->  ESP32-S3   │
│   (leader/follower)        host RPC                (WiFi)   │
└───────────────┬───────────────────────────────┬─────────────┘
                │ UART 1 Mbaud                  │ UART 1 Mbaud
                ▼                               ▼
┌─────────────────────────────────────────────────────────────┐
│                  STM32 有线 mesh 平面                        │
│   link frame v2 + mesh2 control + DATA_MINI9P/RPC/JOB        │
│   节点可作为 relay，多 UART、多跳转发                         │
│                                                              │
│   数据面：                                                   │
│   DATA_MINI9P  → mini9P server → local_vfs                   │
│   DATA_RPC     → rpc_service                                    │
│   DATA_JOB     → job_service → compute_worker                │
└─────────────────────────────────────────────────────────────┘
```

**关键设计**：
- 两个网络平面只在 ESP32 主机汇合。
- 主机间不透传 STM32 link frame；跨主机访问由 owner 主机终止 host RPC，再发向自己的 STM32 子树。
- STM32 不运行全局路由算法，只维护本地转发表；coordinator 计算路由并下发。

---

## 4. 目录结构逐项解释

| 目录 | 作用 |
|---|---|
| `pwos-shared/` | 与具体 HAL/IDF 无关的共享协议和算法 |
| `pwos-shared/link/` | link frame v2、CRC-16/CCITT-FALSE、流式 parser |
| `pwos-shared/mesh2/` | 节点注册、地址租约、链路状态、路由更新、host advertise |
| `pwos-shared/mini9p/` | mini9P 协议编解码、client、server |
| `pwos-shared/rpc/` | MCU RPC 内层协议 |
| `pwos-shared/job/` | MCU Job 内层协议 |
| `pwos-shared/host_rpc/` | ESP32 主机间 CBOR RPC 和 leader election |
| `pwos-shared/render/` | smallpt 光线追踪 tile kernel |
| `pwos-master-esp32p4/` | P4 主机固件：coordinator、WebShell、host RPC、Lua、推理 |
| `pwos-master-esp32s3/` | S3 主机固件：WiFi、host RPC、推理 |
| `pwos-slave/` | STM32F407 FreeRTOS 节点 |
| `pwos-slave-stm32f429/` | STM32F429 节点 + 240x320 LCD |
| `docs/` | 架构、协议、构建、重构记录 |
| `Lab4/` | 独立 Linux 分布式推理实验（llama.cpp + Ray） |

---

## 5. 核心运行流程

### 5.1 系统启动流程

**ESP32-P4**：
1. `app_main()` 打印芯片信息。
2. `pwos_coordinator_runtime_start_default()`：初始化 parser、coordinator、session_manager、rpc_client、job_manager、cluster_vfs、UART1。
3. 创建 `coordinator_task`（主循环）和 `mini9p_probe_task`。
4. 启动 LAN、host RPC、HTTP/WebSocket、Lua、inference。

**STM32F407**：
1. `main()`：HAL_Init、SystemClock_Config、MX_GPIO/DMA/USART/SDIO_Init。
2. `osKernelInitialize()`、`MX_FREERTOS_Init()`。
3. `pwos_queues_init()` 创建队列。
4. `pwos_tasks_start()` 创建 8 个静态任务。
5. `osKernelStart()` 启动调度器。

### 5.2 STM32 节点上线流程

1. **HELLO 发现**：port_manager 周期发送 `LINK_HELLO`，收到 `LINK_HELLO_ACK`。
2. **选择上游**：优先直连 coordinator，其次选择声明 `UPSTREAM_REACHABLE` 的 relay 节点。
3. **发送注册**：node_control 发送 `CTRL_NODE_REGISTER`（含 UID、boot_id、upstream_port）。
4. **地址分配**：coordinator 返回 `CTRL_ADDR_ASSIGN`，节点进入 `ASSIGNED` 状态。
5. **租约续期**：周期发送 `CTRL_LEASE_RENEW`，收到 `CTRL_LEASE_ACK`。
6. **链路状态上报**：周期发送 `CTRL_LINK_STATE`。
7. **路由下发**：coordinator 计算后下发 `CTRL_ROUTE_UPDATE`。

### 5.3 数据面请求流程（以 `cat /mcu1/sys/health` 为例）

1. WebShell/HTTP 调用 `cluster_vfs_read_path("/mcu1/sys/health")`。
2. cluster_vfs 解析路径 → 找到 route `mcu1` → 地址 `addr`。
3. 通过 session_manager 获取或创建 mini9P client。
4. 发送 `TATTACH` → `TWALK /sys/health` → `TOPEN` → `TREAD`。
5. session_manager 把 mini9P 帧作为 `DATA_MINI9P` payload，经 UART 发往 STM32。
6. STM32 node_control 转发或投递到 service_task。
7. service_runtime 调用 `m9p_server_handle_frame()`，local_vfs 的 `service_vfs_read()` 生成 `/sys/health` 文本。
8. 响应沿原路返回，session_manager 按 `(DATA_MINI9P, src_addr, wire_tag)` 匹配 pending。

### 5.4 Job 提交流程

1. 用户：`job submit mcu1 hash hello`。
2. host_shell → job_command → job_manager → session_manager。
3. 发送 `DATA_JOB` `SUBMIT` 到 STM32。
4. job_service 解析，提交给 compute_worker。
5. compute_task 低优先级增量执行。
6. 用户轮询 `job status <id>`，`job result <id>`。

---

## 6. 核心模块详细讲解

### 6.1 pwos-shared/link：链路帧与流式解析器

**职责**：定义 link frame v2 线缆格式、CRC、编解码、字节流解析器。

**帧格式**（19 字节头 + 最多 512 字节 payload）：
```
magic[2]='MH' | version=2 | hdr_len=19 | type | flags | src | dst | ttl | seq[2] | ack[2] | payload_len[2] | hdr_crc[2] | payload_crc[2] | payload
```

**设计亮点**：
- 双层 CRC：头 CRC 覆盖头部，可在收齐头部后立即诊断错误。
- 小端序，与具体 HAL 无关。
- 流式 parser 支持任意分块、噪声重同步、半帧保留。

### 6.2 pwos-shared/mesh2：控制面协议

**消息类型**：
- `NODE_REGISTER`（24B）：UID、boot_id、caps、upstream_port。
- `ADDR_ASSIGN`（28B）：UID、boot_id、lease_epoch、lease_ms、addr。
- `LEASE_RENEW/ACK`：维持租约。
- `LINK_STATE`（24B）：本地地址、端口、对端信息、metric。
- `ROUTE_UPDATE`（12B）：dst、next_hop、metric、version、action。
- `HOST_ADVERTISE`（28B）：host_uid、epoch、cluster_id、priority、role。

### 6.3 pwos-shared/mini9p：远端文件协议

**特点**：
- 精简版 9P：attach、walk、open、read、write、stat、clunk。
- 每个对象用 `qid` 标识。
- 主机路径 `/mcuN/sys/health` 映射为节点本地 `/sys/health`。

### 6.4 ESP32 coordinator_runtime

**核心结构**（`pwos-master-esp32p4/coordinator_runtime/pwos_coordinator_runtime.c`）：
- 一个全局 `g_runtime`。
- UART RX 由 `coordinator_task` 唯一消费。
- 控制帧直接处理；数据面交给 session_manager。
- 每 500ms 发 HELLO，每 5s 打印状态，每 1s 发 host_advertise。

### 6.5 host_coordinator

**职责**：
- 维护节点表（最多 32 个）。
- 按 UID 识别硬件，按 boot_id 识别一次启动。
- 分配地址（1~253，跳过 0x00/0xFF）。
- 处理租约续期。
- 根据 `LINK_STATE` 计算路由。

**路由计算逻辑**：
- 收到 local 上报的到 peer 的链路状态。
- 正向路由：告诉 local 怎么到 peer（next_hop=peer，metric=link.metric+1）。
- 反向路由：告诉 peer 怎么到 local。
- 通过 `ROUTE_UPDATE` 下发。

### 6.6 session_manager

**职责**：并发管理 mini9P/RPC/Job 的 typed pending。

**核心数据结构**：
- `sessions[32]`：每个节点一个 session，含 mini9P client。
- `pending[8]`：等待响应的槽位，键为 `(data_type, src_addr, wire_tag)`。
- 同步回调：lock/unlock、client_lock、pending_wait/signal，由平台注入。

**并发策略**：
- 每个节点有独立 client mutex，不同节点可并发。
- pending 表全局锁保护。
- 响应按 `(data_type, src_addr, wire_tag)` 精确匹配。

### 6.7 cluster_vfs

**职责**：把 `/mcuN/...` 映射到 STM32 本地路径。

**核心机制**：
- `routes[]`：每个在线节点一条 route，含 target（mcuN）、addr、boot_id、generation。
- `files[]`：本地 fd 到 remote_fid 的映射。
- 使用 generation 检测地址/boot_id 变化，防止 fd 指向错误节点。

### 6.8 STM32 port_manager

**职责**：维护物理 UART 端口的 HELLO 状态机。

**状态**：DISABLED、PROBING、LINK_UP、HOST_CANDIDATE、UPSTREAM、PEER、SUSPECT、QUARANTINED。

**选择上游优先级**：
1. 已标记为 UPSTREAM 的端口。
2. HOST_CANDIDATE（直连 coordinator）。
3. 声明 `UPSTREAM_REACHABLE` 的 PEER（可作为 relay）。

### 6.9 STM32 node_control

**职责**：本机地址、租约、上游、路由表、中继转发。

**关键设计**：
- 收到非本地目的地址的帧 → 查路由表 → 递减 TTL → 转发。
- 收到 `CTRL_NODE_REGISTER` 且本机已分配地址 → 记录 pending 并向上游转发（relay）。
- 收到 `CTRL_ADDR_ASSIGN`：
  - 若是自己：应用地址，标记上游。
  - 若是 pending 中的下游：设置直连路由并转发。
- 收到 `HOST_ADVERTISE`：按 `(epoch, priority, host_uid)` 选择 authority，避免三角环抖动（只有 TTL 更大才切换路径）。

### 6.10 STM32 service_runtime

**职责**：本机数据面唯一入口，串行处理 mini9P/RPC/Job。

**初始化**：
- compute_worker（带 mutex）
- job_service
- local_vfs（`/sys/*`、`/compute/*`）
- mini9P server
- rpc_service（内置 system.ping/stream/info/notify/delay/fail）
- 在 `service_task` 中轮询。

### 6.11 compute_worker

**职责**：在低优先级 compute_task 中增量执行 kernel。

**支持的 kernel**：
- hash：FNV-1a 32。
- vector_add：16 位整数向量相加。
- matmul：小矩阵乘法（最多 8x8）。
- Mandelbrot：固定点迭代，生成分形图像。
- raytrace_tile：smallpt 光线追踪，输出 RGB565 tile。

**调度策略**：
- 每次 `pwos_compute_worker_step()` 执行一个 job 的一个 work unit。
- 完成后主动 `vTaskDelay(1ms)`，让高优先级任务抢占。

---

## 7. 关键算法/思想详细讲解

### 7.1 流式帧解析器的状态机

**问题**：UART 接收是字节流，需要把无结构字节还原成完整帧，并处理噪声、半帧、连续帧。

**输入**：任意字节流（来自 DMA 接收块）。

**输出**：FRAME 事件（含解码后的帧视图）或 ERROR 事件。

**核心步骤**（`pwos_link_parser.c`）：
1. `len == 0`：寻找 magic[0] `'M'`。
2. `len == 1`：等待 magic[1] `'H'`；若又来 `'M'` 则保留。
3. `len >= 2`：锁定候选帧，逐字节存入缓冲区。
4. `len == PWOS_LINK_OFF_TYPE`（=5）：校验 version 和 hdr_len。
5. `len == PWOS_LINK_HDR_LEN`（=19）：校验 hdr_crc，读取 payload_len，得到 want_len。
6. `len == want_len`：调用 `pwos_link_decode` 做最终校验，输出 FRAME 事件。
7. 错误时尽量保留尾部 magic 前缀以快速恢复。

**性能特点**：
- 时间复杂度：O(n)，每个字节常数时间处理。
- 空间：固定 531 字节缓冲区，无动态分配。
- 适合 ISR/任务中调用。

**优点**：
- 对噪声鲁棒，不会日志刷屏。
- 锁定帧头后不在 payload 中重新同步，避免误切。

**局限**：
- 只保留最后 1~2 字节 magic 前缀，坏帧跨越多个候选时恢复稍慢。
- payload 阶段出错直接丢弃整帧。

### 7.2 地址分配与租约算法

**问题**：多个 STM32 节点动态加入，需要稳定身份和可复用的短地址。

**输入**：`NODE_REGISTER`（UID、boot_id、upstream_port、caps）。

**输出**：`ADDR_ASSIGN`（addr、lease_epoch、lease_ms）。

**核心步骤**（`host_coordinator.c`）：
1. 按 UID 查找节点。
2. 若新节点：分配下一个可用地址（1~253）。
3. 若 UID 相同但 boot_id 不同：视为重启，保留地址但更新 boot_id 和 lease_epoch。
4. 填充 `ADDR_ASSIGN`，lease_ms 默认 30s。

**租约续期**：
- 节点在 lease_ms/2 时发送 `LEASE_RENEW`。
- coordinator 校验 UID + boot_id + addr + lease_epoch，返回 `LEASE_ACK`。

**优点**：
- UID 标识硬件，boot_id 标识一次启动，旧状态可安全清理。
- 地址耗尽时线性扫描，最多 254 次尝试。

**局限**：
- 地址空间只有 8 位（0x00 host，0xFF 未分配/广播），最多 253 个节点。
- 多主机时 follower 会清空本地地址域，等待 leader 统一命名。

### 7.3 路由计算算法

**问题**：coordinator 知道全局拓扑，需要给每个节点下发到其它节点的路径。

**输入**：`LINK_STATE`（local_addr、local_port、peer_addr、peer_port、metric、peer_uid、peer_boot_id、flags）。

**输出**：一条或两条 `ROUTE_UPDATE`。

**核心步骤**（`host_coordinator_handle_link_state`）：
1. 查找 local 和 peer 的节点表项。
2. 若任一不存在，忽略。
3. 正向路由：dst=peer，next_hop=peer，metric=link.metric+1，action=SET/DELETE。
4. 反向路由：dst=local，next_hop=local，metric=link.metric+1。
5. 返回 1 表示产生新路由，调用方分别发给 local 和 peer。

**在 STM32 节点上的应用**（`node_control.c`）：
- 收到 `ROUTE_UPDATE` 后，比较 route_version，旧版本忽略。
- DELETE 则删除路由；SET 则通过 next_hop 解析端口，写入路由表。

**优点**：
- 节点不跑全局算法，只维护本地表，适合资源受限设备。
- 链路状态驱动，拓扑变化时增量更新。

**局限**：
- 当前 metric 简单基于跳数，未考虑带宽/负载。
- 链路状态周期上报，收敛有延迟。

### 7.4 Typed Pending 匹配算法

**问题**：主机同时向一个节点发送 mini9P、RPC、Job 请求，如何正确匹配响应？

**输入**：发送时记录 `(data_type, dst_addr, wire_tag)`；接收时从响应中解析出 `(data_type, src_addr, wire_tag)`。

**输出**：把响应投递到正确的 pending 槽位，唤醒等待者。

**核心步骤**（`session_manager.c`）：
1. `reserve_pending_locked`：分配 pending 槽位和唯一 wire_tag。
2. 发送请求前调用 `retag` 把 wire_tag 写入帧中。
3. RX task 收到响应后调用 `deliver_data` 或 `deliver_data_part`。
4. 遍历 pending 表，匹配 `data_type`、`src_addr`、`wire_tag`。
5. 复制响应，标记 completed，调用 pending_signal 唤醒。
6. `wait_for_pending` 超时后释放槽位。

**流式响应**：
- `deliver_data_part` 按 chunk 序号顺序追加到固定缓冲。
- 序号不连续则标记错误。
- 只有 STREAM_END 才唤醒等待者。

**优点**：
- 同一节点不同协议可并发（不同 data_type）。
- 精确匹配避免旧响应污染。
- boot_id 变化时 reset_node 取消旧 pending。

### 7.5 增量计算调度算法

**问题**：STM32 计算能力弱，长计算不能阻塞控制面和数据面。

**输入**：Job submit（kernel、input）。

**输出**：Job 状态变化 + 结果。

**核心步骤**（`compute_worker.c`）：
1. `submit` 校验输入，分配静态 job 槽位，状态变为 QUEUED。
2. `compute_task` 每周期调用 `pwos_compute_worker_step()`。
3. 若存在 RUNNING job，执行一个 work unit；否则取最老的 QUEUED job 启动。
4. 每个 kernel 的 `step_*` 函数完成一个像素/一个元素/一次迭代。
5. `work_index == work_total` 时状态变为 DONE。
6. 每步后 `vTaskDelay(1ms)` 让出 CPU。

**优点**：
- 控制面始终可抢占。
- 静态槽位无堆分配。
- 进度可查询（progress_permille）。

**局限**：
- 每个 job 串行执行，未实现优先级或抢占。
- 单个 work unit 仍可能耗时较长（如 matmul 的一个元素）。

---

## 8. 关键代码片段讲解

### 片段 1：coordinator 主循环帧处理

**文件**：`pwos-master-esp32p4/coordinator_runtime/pwos_coordinator_runtime.c`

**功能**：UART RX task 的核心分发逻辑。

**位置**：`handle_frame()` 函数。

```c
static void handle_frame(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view)
{
    int rc;

    ++runtime->stats.rx_frames;

    if (runtime->control_leader == 0u &&
        view->type != PWOS_LINK_TYPE_LINK_HELLO &&
        view->type != PWOS_LINK_TYPE_LINK_HELLO_ACK) {
        ++runtime->stats.nonleader_rx_drop;
        return;
    }

    switch (view->type) {
    case PWOS_LINK_TYPE_LINK_HELLO:
        ++runtime->stats.hello_rx;
        (void)send_hello(runtime, (uint8_t)PWOS_LINK_TYPE_LINK_HELLO_ACK);
        break;
    case PWOS_LINK_TYPE_CTRL_NODE_REGISTER:
        (void)handle_node_register(runtime, view);
        break;
    case PWOS_LINK_TYPE_CTRL_LEASE_RENEW:
        (void)handle_lease_renew(runtime, view);
        break;
    case PWOS_LINK_TYPE_CTRL_LINK_STATE:
        (void)handle_link_state(runtime, view);
        break;
    case PWOS_LINK_TYPE_DATA_MINI9P:
        rc = pwos_session_manager_deliver_mini9p(
            &runtime->sessions,
            view->src,
            view->payload,
            view->payload_len);
        break;
    // ... DATA_RPC, DATA_JOB ...
    }
}
```

**解释**：
- `runtime->control_leader` 为 0 时（当前是 follower），只处理 HELLO/ACK，其它控制帧丢弃。这是多主机时避免脑裂的关键。
- HELLO 直接回复 ACK。
- 控制面帧调用各自 handler。
- 数据面帧按类型交给 session_manager 的 deliver 函数，按 `(src_addr, tag)` 或 `(data_type, src_addr, wire_tag)` 匹配。

### 片段 2：地址分配

**文件**：`pwos-master-esp32p4/host_coordinator/host_coordinator.c`

**功能**：为新注册节点分配 mesh 短地址。

```c
static int alloc_addr(pwos_host_coordinator_t *coordinator, uint8_t *out_addr)
{
    uint16_t tries;
    uint8_t candidate;

    candidate = coordinator->next_addr;
    if (candidate == PWOS_LINK_ADDR_HOST || candidate == PWOS_LINK_ADDR_UNASSIGNED) {
        candidate = 1u;
    }

    for (tries = 0u; tries < 254u; ++tries) {
        if (candidate == PWOS_LINK_ADDR_HOST || candidate == PWOS_LINK_ADDR_UNASSIGNED) {
            candidate = 1u;
        }
        if (addr_in_use(coordinator, candidate) == 0u) {
            *out_addr = candidate;
            coordinator->next_addr = (uint8_t)(candidate + 1u);
            return 0;
        }
        ++candidate;
    }
    return -1;
}
```

**解释**：
- 从 `next_addr` 开始线性扫描。
- 跳过 0x00（host）和 0xFF（未分配/广播）。
- 最多 254 次尝试，保证找到可用地址或确定已满。
- 分配后更新 `next_addr`，下次从这个地址之后继续扫描。

### 片段 3：pending 匹配

**文件**：`pwos-master-esp32p4/host_sessions/session_manager.c`

**功能**：把收到的响应匹配到等待的 pending 槽位。

```c
manager_lock(manager);
for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
    pwos_session_pending_t *pending = &manager->pending[i];

    if (pending->used == 0u || pending->completed != 0u ||
        pending->streaming != 0u ||
        pending->data_type != data_type || pending->src_addr != src_addr ||
        pending->wire_tag != wire_tag) {
        continue;
    }
    if (pending->session_index >= PWOS_SESSION_MANAGER_MAX_SESSIONS ||
        manager->sessions[pending->session_index].boot_id != pending->boot_id ||
        manager->sessions[pending->session_index].resetting != 0u) {
        pending->status = PWOS_SESSION_ERR_STALE_BOOT;
    } else {
        memcpy(pending->response, payload, payload_len);
        pending->response_len = (uint16_t)payload_len;
        pending->status = 0;
    }
    pending->completed = 1u;
    signal_index = (uint8_t)i;
    ++manager->stats.pending_delivered;
    break;
}
manager_unlock(manager);

if (signal_index != 0xFFu && manager->config.pending_signal != NULL) {
    manager->config.pending_signal(manager->config.sync_ctx, signal_index);
}
```

**解释**：
- 遍历 pending 表，严格匹配 `data_type`、`src_addr`、`wire_tag`。
- 同时校验 session 的 boot_id 是否与 pending 记录一致；不一致说明节点重启，标记 STALE_BOOT。
- 复制响应后标记 completed，调用 signal 唤醒等待任务。
- 锁只保护临界区，signal 在解锁后调用，避免唤醒后立即抢锁的上下文切换开销。

### 片段 4：cluster_vfs 路径解析

**文件**：`pwos-master-esp32p4/host_api/cluster_vfs.c`

**功能**：把 `/mcuN/...` 解析为 remote path 和 route reference。

```c
vfs_lock(vfs);
for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
    target_len = strlen(vfs->routes[i].target);
    path_target = path + 1u;
    if (strncmp(path_target, vfs->routes[i].target, target_len) != 0) {
        continue;
    }
    if (path_target[target_len] != '\0' && path_target[target_len] != '/') {
        continue;
    }
    if (route_is_online(&vfs->routes[i]) == 0u) {
        matched_offline = 1u;
        continue;
    }

    mapped_path = path_target + target_len;
    if (mapped_path[0] == '\0') {
        mapped_path = "/";
    }
    memcpy(remote_path, mapped_path, strlen(mapped_path) + 1u);
    route_to_ref(vfs, &vfs->routes[i], out_ref);
    vfs_unlock(vfs);
    rc = ensure_attached(vfs, out_ref, deadline_ms);
    return rc;
}
vfs_unlock(vfs);
return matched_offline != 0u ? PWOS_SESSION_ERR_NO_ROUTE : -(int)M9P_ERR_ENOENT;
```

**解释**：
- 遍历 routes，匹配 `/mcuN` 前缀。
- 检查路径边界（必须是完整 target 或后跟 `/`）。
- 若节点离线，记录 `matched_offline`，最后返回 NO_ROUTE。
- 把 `/mcuN/sys/health` 映射为 `/sys/health`。
- 返回前调用 `ensure_attached` 触发 mini9P attach。

### 片段 5：STM32 路由转发

**文件**：`pwos-slave/User/mesh2/node_control.c`

**功能**：中继数据面/控制面帧。

```c
static int forward_view(
    const pwos_link_frame_view_t *view,
    uint8_t port_id)
{
    pwos_frame_block_t *block;
    size_t frame_len = 0u;
    pwos_status_t status;

    if (view == NULL || view->ttl <= 1u) {
        ++g_stats.drop_no_route;
        return -1;
    }

    block = pwos_frame_pool_alloc();
    // ...
    status = pwos_link_encode(
        view->type,
        view->flags,
        view->src,
        view->dst,
        (uint8_t)(view->ttl - 1u),
        view->seq,
        view->ack,
        view->payload,
        view->payload_len,
        block->data,
        sizeof(block->data),
        &frame_len);
    // ...
    block->port_id = port_id;
    if (pwos_link_type_is_control(view->type)) {
        pwos_ctrl_tx_send(block, 0u);
    } else {
        pwos_link_tx_send(block, 0u);
    }
    return 0;
}
```

**解释**：
- TTL <= 1 时丢弃，防止环路。
- 从固定帧池分配 block，避免动态内存。
- 重新编码帧，TTL 减 1。
- 控制面帧进 `ctrl_tx` 队列（高优先级），数据面帧进 `link_tx` 队列。

### 片段 6：compute_worker 增量执行

**文件**：`pwos-slave/User/compute/compute_worker.c`

**功能**：每次执行一个 job 的一个 work unit。

```c
int pwos_compute_worker_step(pwos_compute_worker_t *worker)
{
    // ... 找到 RUNNING 或最老 QUEUED job ...
    switch (job->kernel) {
    case PWOS_JOB_KERNEL_HASH: step_hash(job); break;
    case PWOS_JOB_KERNEL_VECTOR_ADD: step_vector_add(job); break;
    case PWOS_JOB_KERNEL_MATMUL: step_matmul(job); break;
    case PWOS_JOB_KERNEL_MANDELBROT: step_mandelbrot(job); break;
    case PWOS_JOB_KERNEL_RAYTRACE_TILE: step_raytrace_tile(job); break;
    default:
        job->state = PWOS_JOB_STATE_FAILED;
    }
    ++worker->stats.steps;
    if (job->state == PWOS_JOB_STATE_RUNNING) {
        job->progress_permille = (uint16_t)(
            (job->work_index * PWOS_JOB_PROGRESS_MAX) / job->work_total);
        if (job->work_index >= job->work_total) {
            job->state = PWOS_JOB_STATE_DONE;
            job->progress_permille = PWOS_JOB_PROGRESS_MAX;
        }
    }
    worker_unlock(worker);
    return 1;
}
```

**解释**：
- 每个 kernel 的 step 只处理一个单元。
- 更新 progress_permille（千分比）。
- 完成后状态变为 DONE。
- 由低优先级 compute_task 周期调用，保证控制面不被阻塞。

---

## 9. 数据流与调用链分析

### 9.1 主机发送 mini9P read 的完整调用链

```
WebShell/HTTP
  → pwos_cluster_vfs_read_path()
    → pwos_cluster_vfs_open()
      → resolve_path() → session_manager_attach()
        → m9p_client_attach()
          → session_transport()
            → reserve_pending_locked() + m9p_retag_frame()
            → session_send_data() → send_mesh2_payload() → uart_write_bytes()
    → pwos_cluster_vfs_read()
      → m9p_client_read() → session_transport() → uart_write_bytes()
    → pwos_cluster_vfs_close()
```

### 9.2 STM32 接收并响应的完整调用链

```
UART IDLE ISR
  → pwos_link_rx_send_from_isr()
    → link_rx_task
      → pwos_port_manager_handle_rx() （HELLO 帧）
      → pwos_mesh_rx_send()
        → mesh_ctrl_task
          → pwos_node_control_handle_rx()
            → 本机目的 → pwos_service_runtime_accepts()
              → pwos_service_rx_send()
                → service_task
                  → pwos_service_runtime_process()
                    → m9p_server_handle_frame()
                      → local_vfs read/write
                    → pwos_node_control_send_data()
                      → send_encoded_data() → pwos_link_tx_send()
                        → link_tx_task → pwos_uart_dma_send()
```

### 9.3 Job 提交调用链

```
host_shell command
  → pwos_job_command_execute()
    → pwos_job_manager_submit()
      → pwos_session_manager_request_data()
        → send DATA_JOB SUBMIT
          → STM32 job_service → compute_worker submit
```

---

## 10. 工程设计亮点

1. **控制面与数据面严格分离**
   - 独立队列：`ctrl_tx` 优先级高于 `link_tx`。
   - 独立任务：coordinator_task 处理控制面，session 处理数据面。
   - 文档约束：控制面不能因文件访问、RPC、Job 阻塞。

2. **零动态内存设计**
   - STM32 使用静态任务、静态帧池、静态 job 槽位。
   - 主机 session_manager 使用固定大小的 sessions 和 pending 数组。
   - 适合资源受限设备，避免堆碎片和分配失败。

3. **状态机驱动的链路发现**
   - port_manager 的 HELLO FSM 清晰处理 PROBING/UPSTREAM/SUSPECT 等状态。
   - node_control 的 authority 选择避免三角环抖动。

4. **Typed Pending 精确匹配**
   - 按 `(data_type, src_addr, wire_tag)` 匹配，避免协议间干扰。
   - boot_id 变化时自动清理旧 pending 和 session。

5. **Generation 机制保证一致性**
   - cluster_vfs 的 route_generation 和 file_generation 检测节点身份变化。
   - 防止地址复用后 fd 指向错误节点。

6. **可测试性**
   - 共享协议模块都有 PC 单元测试。
   - session_manager 的同步回调可注入，PC 测试可同步投递响应。

7. **多主机一致性**
   - leader election 按 `(epoch, priority, host_uid)` 排序。
   - follower 清空本地地址域，避免同一 MCU 同时存在于两个地址域。

---

## 11. 缺点与改进方向

| 缺点 | 改进方向 |
|---|---|
| 地址空间只有 8 位，最多 253 个节点 | 扩展为 16 位地址或分层地址 |
| 路由 metric 仅基于跳数 | 引入带宽、负载、延迟等加权 metric |
| 当前不支持数据面分片/BULK | 实现 `DATA_BULK` 分片传输，支持大文件 |
| 主机间无 TLS/鉴权 | 添加预共享密钥或证书 |
| Job 调度无优先级和抢占 | 增加优先级队列、时间片、抢占点 |
| 推理服务仍是骨架 | 完成 host RPC 推理协作，集成到 MCU Job 或 llama.cpp RPC |
| 无持久化配置 | NVS/Flash 保存节点别名、网络配置 |
| 当前跨主机只支持 read/write | 支持 list/stat 和目录操作 |
| 诊断依赖文本输出 | 增加结构化 CBOR 诊断接口 |

---

## 12. 40 分钟 PPT 页面规划

共 30 页，按 40 分钟分配：

| 页码 | 内容 | 时间 |
|---|---|---|
| 1 | 封面 | 1 min |
| 2 | 项目概述与一句话概括 | 1 min |
| 3 | 项目背景与应用场景 | 2 min |
| 4 | 要解决的核心问题 | 2 min |
| 5 | 整体架构图 | 3 min |
| 6 | 技术栈与依赖 | 1 min |
| 7 | 目录结构 | 1 min |
| 8 | 启动流程（ESP32 + STM32） | 2 min |
| 9 | STM32 节点上线流程 | 3 min |
| 10 | 数据面请求流程（示例） | 2 min |
| 11 | 核心模块一：link frame v2 + parser | 3 min |
| 12 | 核心模块二：mesh2 控制面 | 2 min |
| 13 | 核心模块三：mini9P 与 cluster_vfs | 3 min |
| 14 | 核心模块四：session_manager 与 typed pending | 3 min |
| 15 | 核心模块五：host_coordinator 与路由 | 2 min |
| 16 | 核心模块六：STM32 node_control | 2 min |
| 17 | 核心模块七：service_runtime + compute_worker | 2 min |
| 18 | 核心模块八：host RPC 与 leader election | 2 min |
| 19 | 关键算法一：流式帧解析状态机 | 2 min |
| 20 | 关键算法二：地址分配与租约 | 1 min |
| 21 | 关键算法三：路由计算 | 1 min |
| 22 | 关键算法四：typed pending 匹配 | 1 min |
| 23 | 关键算法五：增量计算调度 | 1 min |
| 24 | 代码片段讲解（2-3 段） | 3 min |
| 25 | 数据流与调用链 | 2 min |
| 26 | 工程设计亮点 | 2 min |
| 27 | 异常处理与边界情况 | 1 min |
| 28 | 性能、复杂度与可扩展性 | 1 min |
| 29 | 不足与改进方向 | 1 min |
| 30 | 总结与 Q&A | 2 min |

---

## 13. 每页 PPT 的文字材料与讲稿

### 第 1 页：封面

**PPT 页面文字**：
- 标题：9P-Walkers：异构 MCU 集群实验系统
- 副标题：基于 ESP32 与 STM32 的分布式计算与协议栈
- 姓名/日期

**讲解稿**：
> 各位老师好，我今天答辩的题目是 9P-Walkers，一个面向异构 MCU 集群的实验系统。我们的目标是在资源受限的嵌入式设备上，构建一个统一的命名、通信和计算调度平台。

**可视化建议**：项目 Logo（如有）、系统拓扑缩略图。

**预计讲解时间**：1 分钟。

---

### 第 2 页：项目概述

**PPT 页面文字**：
- 一句话概括
- 两大网络平面：主机间 IP 平面 + STM32 有线 mesh 平面
- 三大数据面：mini9P、RPC、Job

**讲解稿**：
> 9P-Walkers 用 ESP32 作主机、STM32 作节点，通过 UART 组成多跳 mesh，再经主机间 TCP/CBOR 互联。对上层只暴露一个统一的 `/mcuN/...` 命名空间，支持文件读写、RPC 调用和异步计算任务。

**可视化建议**：双平面架构简图。

**预计讲解时间**：1 分钟。

---

### 第 3 页：项目背景与应用场景

**PPT 页面文字**：
- 背景：物联网节点孤立、缺乏统一集群抽象
- 场景：边缘计算、远程诊断、分布式渲染、LLM 推理实验

**讲解稿**：
> 传统嵌入式节点各自为战，我们希望它们能像小型集群一样协同。应用场景包括把计算任务分发到多个 MCU、通过统一路径读取节点健康状态、以及用多主机扩展规模。

**可视化建议**：应用场景图标。

**预计讲解时间**：2 分钟。

---

### 第 4 页：核心问题

**PPT 页面文字**：
- 如何统一命名与访问？
- 如何自动发现多跳拓扑？
- 控制面与数据面如何隔离？
- 主机间如何协作？
- 长计算如何不阻塞通信？

**讲解稿**：
> 本项目要解决五个核心问题：命名、拓扑发现、控制/数据隔离、主机协作、计算调度。每个问题都对应了具体的协议和模块设计。

**可视化建议**：五个问题对应五个模块的映射图。

**预计讲解时间**：2 分钟。

---

### 第 5 页：整体架构

**PPT 页面文字**：
- 主机间 IP 平面：mDNS + TCP/9909 + CBOR
- STM32 有线 mesh：link v2 + mesh2
- 数据面：DATA_MINI9P / DATA_RPC / DATA_JOB

**讲解稿**：
> 系统有两个网络平面。主机间跑 mDNS 发现和 TCP/9909 CBOR RPC；STM32 之间跑我们自定义的 link frame v2，上面承载 mesh2 控制面和三个数据面协议。两个平面只在 ESP32 主机汇合，跨主机访问由 owner 主机代理。

**可视化建议**：完整架构图，用不同颜色区分平面。

**预计讲解时间**：3 分钟。

---

### 第 6 页：技术栈

**PPT 页面文字**：
- ESP32-P4/S3 + ESP-IDF 6.0 + FreeRTOS
- STM32F407/F429 + HAL + FreeRTOS
- CMake / OpenOCD / idf.py
- C11，无 C++

**讲解稿**：
> 主机端用 ESP-IDF 6.0，节点端用 STM32 HAL + FreeRTOS，全部用 C11 编写。构建上 STM32 用 CMake + OpenOCD，ESP32 用 idf.py。

**可视化建议**：技术栈分层图。

**预计讲解时间**：1 分钟。

---

### 第 7 页：目录结构

**PPT 页面文字**：
- pwos-shared：共享协议
- pwos-master-esp32p4/p4：P4 主机
- pwos-master-esp32s3：S3 主机
- pwos-slave/pwos-slave-stm32f429：节点
- Lab4：独立推理实验

**讲解稿**：
> 仓库按角色拆分。pwos-shared 放与平台无关的协议；两个 master 目录放主机固件；两个 slave 目录放节点固件；Lab4 是独立的 Linux 分布式推理实验。

**可视化建议**：目录树。

**预计讲解时间**：1 分钟。

---

### 第 8 页：启动流程

**PPT 页面文字**：
- ESP32：app_main → coordinator_runtime_start → 创建任务 → 启动网络/Shell/Lua/推理
- STM32：main → HAL init → FreeRTOS init → 创建队列和任务 → 启动调度器

**讲解稿**：
> P4 从 app_main 启动，先初始化协调器运行时和 UART，再启动网络、WebShell、Lua 调度器和推理服务。STM32 则是典型的 HAL + FreeRTOS 启动流程，创建 8 个固定任务后进入调度器。

**可视化建议**：启动时序图。

**预计讲解时间**：2 分钟。

---

### 第 9 页：STM32 节点上线流程

**PPT 页面文字**：
1. HELLO/ACK 发现
2. 选择上游
3. NODE_REGISTER
4. ADDR_ASSIGN
5. LEASE_RENEW/ACK
6. LINK_STATE 上报
7. ROUTE_UPDATE 下发

**讲解稿**：
> 节点上电后先通过 HELLO 发现邻居，选择一个上游端口，然后发送注册请求。coordinator 分配地址和租约后，节点进入 ASSIGNED 状态，周期续租并上报链路状态，最后接收路由更新。

**可视化建议**：状态转换图。

**预计讲解时间**：3 分钟。

---

### 第 10 页：数据面请求流程

**PPT 页面文字**：
- 示例：`cat /mcu1/sys/health`
- 路径解析 → attach → walk → open → read → clunk
- 响应按 typed pending 匹配

**讲解稿**：
> 以读取健康状态为例，cluster_vfs 把 `/mcu1/sys/health` 解析为节点地址和本地路径，session_manager 走 mini9P 协议发送请求，STM32 的 service_runtime 生成文本后返回。

**可视化建议**：序列图。

**预计讲解时间**：2 分钟。

---

### 第 11 页：核心模块一 — link frame v2 + parser

**PPT 页面文字**：
- 19 字节头 + 最多 512 字节 payload
- 双层 CRC：头 CRC + payload CRC
- 流式解析器：状态机 + 噪声重同步

**讲解稿**：
> link frame v2 是我们自定义的链路帧。19 字节头部包含地址、类型、TTL、序列号等，后面跟 payload。双层 CRC 让我们收到头部就能判断头是否损坏。parser 是一个纯状态机，能从任意字节流中恢复同步。

**可视化建议**：帧格式图、parser 状态机图。

**预计讲解时间**：3 分钟。

---

### 第 12 页：核心模块二 — mesh2 控制面

**PPT 页面文字**：
- 注册、租约、链路状态、路由更新、host advertise
- 固定长度 payload，小端序
- 节点身份 = UID（硬件）+ boot_id（一次启动）

**讲解稿**：
> mesh2 控制面用固定长度消息，适合 MCU 解析。节点身份由 UID 和 boot_id 共同决定，UID 标识硬件，boot_id 标识一次启动，这样节点重启后旧状态可以安全清理。

**可视化建议**：消息类型表。

**预计讲解时间**：2 分钟。

---

### 第 13 页：核心模块三 — mini9P 与 cluster_vfs

**PPT 页面文字**：
- mini9P：attach/walk/open/read/write/stat/clunk
- cluster_vfs：把 `/mcuN/...` 映射到节点本地路径
- generation 防止 fd 指向错误节点

**讲解稿**：
> mini9P 是我们设计的精简版 9P 文件协议。cluster_vfs 在主机侧维护 `/mcuN` 到 mesh 地址的映射，并用 generation 检测节点重启或地址复用，避免文件描述符指向错误节点。

**可视化建议**：路径映射示意图。

**预计讲解时间**：3 分钟。

---

### 第 14 页：核心模块四 — session_manager

**PPT 页面文字**：
- typed pending：按 `(data_type, src_addr, wire_tag)` 匹配
- 每个节点独立 session，不同节点可并发
- boot_id 变化时清理旧 pending

**讲解稿**：
> session_manager 是并发核心。它维护最多 32 个 session 和 8 个 pending 槽位。pending 按数据类型、源地址和标签匹配，这样同一节点的 mini9P、RPC、Job 可以并发而不混淆。

**可视化建议**：pending 表结构图。

**预计讲解时间**：3 分钟。

---

### 第 15 页：核心模块五 — host_coordinator

**PPT 页面文字**：
- 维护节点表、分配地址、处理租约
- 根据 LINK_STATE 计算路由
- 正向路由 + 反向路由同时下发

**讲解稿**：
> host_coordinator 是控制面权威。它维护最多 32 个节点，线性扫描分配地址，根据上报的链路状态计算路由，并同时下发正向和反向路由，让两个节点都知道如何到达对方。

**可视化建议**：路由计算示例图。

**预计讲解时间**：2 分钟。

---

### 第 16 页：核心模块六 — STM32 node_control

**PPT 页面文字**：
- 本机地址、上游、路由表
- 中继转发：查表 → TTL-1 → 发送
- HOST_ADVERTISE 选择 authority，避免三角环抖动

**讲解稿**：
> node_control 运行在 STM32 上，负责维护本机地址和上游，以及本地转发表。收到非本地目的地址的帧时查表转发。对于多主机场景，它通过 HOST_ADVERTISE 选择 authority，只有 TTL 更大才切换路径，避免三角环抖动。

**可视化建议**：转发流程图。

**预计讲解时间**：2 分钟。

---

### 第 17 页：核心模块七 — service_runtime + compute_worker

**PPT 页面文字**：
- service_runtime：串行处理 mini9P/RPC/Job
- compute_worker：低优先级增量执行 kernel
- kernel：hash、vector_add、matmul、mandelbrot、raytrace_tile

**讲解稿**：
> service_runtime 是数据面唯一入口，串行处理请求但不执行长计算。计算交给 compute_worker，在低优先级任务中每次执行一个 work unit，保证控制面不被阻塞。

**可视化建议**：任务优先级图、kernel 列表。

**预计讲解时间**：2 分钟。

---

### 第 18 页：核心模块八 — host RPC

**PPT 页面文字**：
- TCP/9909 + CBOR
- mDNS 发现 `_pwos._tcp`
- leader election：按 `(epoch, priority, host_uid)`
- 跨主机 read/write、拓扑同步、分布式推理

**讲解稿**：
> 主机间通过 TCP/9909 跑 CBOR RPC。用 mDNS 发布 `_pwos._tcp` 服务发现对端。leader 按 epoch、priority 和 UID 选举产生，负责全局命名。 follower 接受 leader 的全局节点表。

**可视化建议**：主机间拓扑图。

**预计讲解时间**：2 分钟。

---

### 第 19 页：关键算法一 — 流式帧解析

**PPT 页面文字**：
- 状态机：找 magic → 收头 → 校验头 CRC → 算 want_len → 收 payload → 最终解码
- 错误时保留尾部 magic 前缀快速恢复

**讲解稿**：
> parser 的核心是线性状态机。收到 `'M'` 后进入候选帧状态，头部收齐后校验 CRC 并计算完整长度，payload 收齐后做最终解码。出错时尽量保留尾部 magic，让下一帧快速恢复。

**可视化建议**：状态机图。

**预计讲解时间**：2 分钟。

---

### 第 20 页：关键算法二 — 地址分配与租约

**PPT 页面文字**：
- 线性扫描 1~253 分配地址
- UID 识别硬件，boot_id 识别启动
- 租约续期在 lease_ms/2 时发起

**讲解稿**：
> 地址分配采用线性扫描，最多 254 次尝试。通过 UID 和 boot_id 区分硬件和启动实例。租约默认 30 秒，节点在 15 秒时发起续期，coordinator 验证后确认。

**可视化建议**：地址分配流程图。

**预计讲解时间**：1 分钟。

---

### 第 21 页：关键算法三 — 路由计算

**PPT 页面文字**：
- 输入：LINK_STATE
- 输出：ROUTE_UPDATE（正向 + 反向）
- STM32 本地表按 version 更新，旧 version 忽略

**讲解稿**：
> coordinator 收到链路状态后，生成两条路由更新：一条告诉上报节点怎么到对端，另一条告诉对端怎么到上报节点。节点端用 version 字段保证只接受新路由。

**可视化建议**：链路状态到路由更新的示意图。

**预计讲解时间**：1 分钟。

---

### 第 22 页：关键算法四 — typed pending 匹配

**PPT 页面文字**：
- 键：`(data_type, src_addr, wire_tag)`
- 支持普通响应和流式分片
- boot_id 校验防止旧响应污染

**讲解稿**：
> typed pending 是主机的并发核心。发送请求时分配唯一 wire_tag 并记录三元组；收到响应时严格匹配。流式响应按 chunk 序号顺序追加，保证完整性。

**可视化建议**：pending 匹配流程图。

**预计讲解时间**：1 分钟。

---

### 第 23 页：关键算法五 — 增量计算调度

**PPT 页面文字**：
- 低优先级 compute_task
- 每个 step 执行一个 work unit
- 完成后 vTaskDelay 让出 CPU
- 支持进度查询

**讲解稿**：
> 计算任务不会一次性执行完。compute_task 每次只走一步，然后主动让出 CPU，这样 UART 接收和控制面任务始终可以抢占，系统不会因为长计算而卡住。

**可视化建议**：compute_task 与其它任务优先级对比图。

**预计讲解时间**：1 分钟。

---

### 第 24 页：代码片段讲解

**PPT 页面文字**：
- 片段 1：coordinator 帧分发
- 片段 2：地址分配线性扫描
- 片段 3：pending 匹配

**讲解稿**：
> 这里展示三段关键代码。第一段是 coordinator 主循环如何分发控制面和数据面帧。第二段是地址分配的线性扫描逻辑。第三段是 session_manager 如何精确匹配响应。

**可视化建议**：三段代码高亮，每段 5-10 行。

**预计讲解时间**：3 分钟。

---

### 第 25 页：数据流与调用链

**PPT 页面文字**：
- 主机发送 mini9P read 的调用链
- STM32 接收并响应的调用链
- Job 提交调用链

**讲解稿**：
> 这一页展示三条完整调用链：主机如何发送文件读取请求、STM32 如何从 ISR 一路处理到 service_task 再返回、以及 Job 如何提交到计算 worker。

**可视化建议**：三张序列图。

**预计讲解时间**：2 分钟。

---

### 第 26 页：工程设计亮点

**PPT 页面文字**：
- 控制面/数据面分离
- 零动态内存
- 状态机驱动发现
- typed pending
- generation 一致性
- 可测试性
- 多主机一致性

**讲解稿**：
> 本项目的工程亮点包括：控制面与数据面严格分离、STM32 端零动态内存、状态机驱动的链路发现、typed pending 精确匹配、generation 机制保证一致性、以及充分的 PC 单元测试。

**可视化建议**：七点列表，每项配图标。

**预计讲解时间**：2 分钟。

---

### 第 27 页：异常处理与边界情况

**PPT 页面文字**：
- CRC 错误丢弃单帧
- 无路由返回 E_NO_ROUTE
- TTL 耗尽丢弃
- boot_id 变化清理旧状态
- pending 超时释放

**讲解稿**：
> 系统在异常处理上做了明确设计：CRC 错只丢当前帧；无路由立即报错；TTL 耗尽丢弃；节点重启后 boot_id 变化，旧 pending、fid、job 全部失效；pending 超时会自动释放槽位。

**可视化建议**：异常处理决策树。

**预计讲解时间**：1 分钟。

---

### 第 28 页：性能、复杂度与可扩展性

**PPT 页面文字**：
- 帧解析 O(n)，空间固定
- 地址分配 O(254)
- 路由查找 O(路由表大小)
- 可扩展：多主机、多跳、新 kernel、新数据类型

**讲解稿**：
> 性能上，帧解析是线性时间，地址分配最多扫描 254 次，路由查找是常数级小表扫描。可扩展性上，系统支持多主机、多跳 relay、新增计算 kernel 和数据类型。

**可视化建议**：性能指标表。

**预计讲解时间**：1 分钟。

---

### 第 29 页：不足与改进方向

**PPT 页面文字**：
- 地址空间 8 位限制
- 路由 metric 仅跳数
- 无 BULK 分片
- 主机间无安全
- Job 无优先级
- 推理服务待完善

**讲解稿**：
> 当前系统还有一些不足：地址空间只有 8 位；路由 metric 只考虑跳数；大数据传输未实现；主机间缺乏安全机制；Job 调度没有优先级；分布式推理服务还是骨架。

**可视化建议**：改进路线图。

**预计讲解时间**：1 分钟。

---

### 第 30 页：总结与 Q&A

**PPT 页面文字**：
- 9P-Walkers = 异构 MCU 集群 + 统一命名 + 多跳 mesh + 分布式计算
- 实现了完整的控制面、数据面、主机间协作
- 谢谢，欢迎提问

**讲解稿**：
> 总结来说，9P-Walkers 实现了一个异构 MCU 集群的完整协议栈和运行时，包括控制面、数据面、主机间协作以及计算调度。虽然还有改进空间，但已经能够支撑统一的远程访问、RPC、Job 和分布式实验。谢谢各位老师！

**可视化建议**：项目 Logo、联系方式。

**预计讲解时间**：2 分钟。

---

## 14. 答辩可能问题与回答

### 问题 1：节点重启后，主机如何知道它是新实例？

**回答**：
节点身份由 UID + boot_id 组成。UID 是 STM32 硬件唯一标识（`HAL_GetUIDw0/1/2`），boot_id 是每次启动生成的随机值（`g_local_boot_id` 由 UID 异或得到）。`NODE_REGISTER` 同时携带两者。主机 `host_coordinator_handle_register` 发现 UID 相同但 boot_id 不同，会更新 boot_id 和 lease_epoch，并触发 `pwos_job_manager_mark_node_lost` 和 `pwos_session_manager_reset_node`，清理旧 session、fid、pending 和 job。

### 问题 2：为什么要用双层 CRC？

**回答**：
头 CRC 覆盖 magic 到 payload_len，payload CRC 只覆盖 payload。这样收到 19 字节头部后就能立即判断头部是否损坏，不必等待整个帧。对于 UART 这种可能有噪声的链路，可以尽快丢弃坏帧并保留 magic 前缀恢复同步。

### 问题 3：多主机时如何避免脑裂？

**回答**：
主机间通过 mDNS 发现，按 `(epoch, priority, host_uid)` 选举 leader。epoch 是持久化并在启动时递增的主机代次。coordinator_runtime 在 `reconcile_control_role()` 中查询 host_rpc 的角色；如果本地变成 follower，会立即清空本地节点表和地址域，避免同一 MCU 同时存在于两个地址域。STM32 端也通过 `HOST_ADVERTISE` 选择 authority，并只接受来自 authority 端口的控制帧。

### 问题 4：控制面为什么不会被数据面阻塞？

**回答**：
STM32 端有独立任务和队列：uart_rx、link_rx、link_tx、port_mgr、mesh_ctrl 优先级较高；service 任务次之；compute 任务最低。控制面帧进 `ctrl_tx` 高优先级队列。长计算在 compute_task 中增量执行，每步后主动让出 CPU。主机端 coordinator_task 只处理控制面和分发数据面，实际数据面 I/O 由 session_manager 的 pending 机制异步完成。

### 问题 5：session_manager 的 pending 表只有 8 个槽位，会不会成为瓶颈？

**回答**：
8 个槽位是全局共享的，但每个节点有独立 session。由于请求通常等待响应后才发下一个，实际并发度受限于应用层。设计上用固定数组避免动态内存，适合资源受限场景。如果后续需要更高并发，可以增大 `PWOS_SESSION_MANAGER_MAX_PENDING` 或按 session 分配 pending。

### 问题 6：/mcuN 名称是如何分配的？

**回答**：
单主机时由本机 cluster_vfs 按发现顺序分配 `mcu1`、`mcu2`...（`allocate_target_name`）。多主机时由 leader 汇总所有 owner 节点的快照后分配全局名称，follower 同步后使用。

### 问题 7：Job 的取消是如何实现的？

**回答**：
主机发送 `CANCEL_REQUEST`，STM32 job_service 调用 `pwos_compute_worker_cancel`。若 job 处于 QUEUED 或 RUNNING，状态直接改为 CANCELLED。由于计算在 compute_task 中每步检查状态，RUNNING 的 job 下一步就会发现被取消。

### 问题 8：流式 RPC 如何保证顺序？

**回答**：
RPC 流式响应的每个 chunk 带有从 0 开始的序号（`status` 字段）。`session_manager_deliver_data_part` 校验 `status_or_part_index == pending->stream_parts`，不连续则标记错误。只有 STREAM_END 才唤醒等待者并返回。

### 问题 9：Lab4 和主项目的关系是什么？

**回答**：
Lab4 是独立的 Linux 分布式推理实验，使用 llama.cpp 和 Ray。它验证了单机部署参数调优、RPC 多机推理、Ray 批量调度和失败重试。主项目的 `pwos-master-esp32p4/inference/` 有本地 LLM 引擎和分布式推理服务骨架，但当前能力较实验性，Lab4 的经验可用于后续完善主机间推理协作。

### 问题 10：你的贡献是什么？

**回答建议**：
> 我的贡献集中在 xx 模块（请根据自身实际填写）。具体包括：设计/实现了 xxx，解决了 xxx 问题，编写了对应单元测试，并在上板实验中验证了 xxx。例如，如果负责 session_manager，可以强调 typed pending 设计、boot_id 一致性处理、以及 PC 测试覆盖。

---

## 15. 不同长度答辩版本

### 3 分钟极简版

> 9P-Walkers 是一个异构 MCU 集群系统。ESP32 主机通过 UART 管理 STM32 节点组成多跳 mesh，主机间通过 TCP/CBOR 互联。系统暴露统一的 `/mcuN/...` 命名空间，支持远端文件、RPC 和异步 Job。核心设计包括：自定义 link frame v2 + 流式解析器、mesh2 控制面、mini9P 文件协议、typed pending 并发机制、以及低优先级增量计算调度。项目还包含 Lab4 的 llama.cpp + Ray 分布式推理实验。

### 10 分钟常规版

> （在 3 分钟版基础上增加）系统分为两个网络平面：主机间 IP 平面和 STM32 有线 mesh 平面。主机负责地址租约、拓扑和路由计算；STM32 只维护本地邻居和转发表。数据面有三种：mini9P 用于文件/诊断，RPC 用于短调用和流式调用，Job 用于异步计算。session_manager 使用 typed pending 按 `(data_type, src_addr, wire_tag)` 匹配响应，支持并发。compute_worker 在低优先级任务中增量执行 kernel，避免阻塞控制面。多主机场景通过 mDNS 发现、epoch 选举 leader、全局命名节点。

### 40 分钟完整版

> 按新版约 45 页 PPT 逐页讲解，覆盖项目概述、整体架构、核心运行流程、协议栈详解（link/mesh2/mini9P/RPC/Job/host RPC）、计算任务系统、关键算法、代码实现与数据流、错误处理与容错、工程亮点与实验验证、不足与改进、总结。重点突出协议字节布局、计算任务状态机与调用链。

---

## 16. 答辩开场白

> 各位老师好，我是 xxx，今天答辩的题目是《9P-Walkers：异构 MCU 集群实验系统》。在接下来的 40 分钟里，我将从项目背景、整体架构、核心协议与模块、关键算法、代码实现、数据流、工程亮点以及不足与改进方向等方面进行介绍。请各位老师批评指正。

---

## 17. 答辩结尾总结

> 总结来说，9P-Walkers 实现了一个面向异构 MCU 集群的完整实验系统，包括 ESP32 主机、STM32 节点、自定义链路协议、控制面与数据面分离、统一命名空间、RPC/Job 机制、多主机协作，以及分布式推理实验。系统在资源受限设备上实现了统一的远程访问和计算调度，具有良好的可扩展性和可测试性。当然，项目在地址空间、路由 metric、安全机制和推理服务等方面仍有改进空间。感谢各位老师的聆听，欢迎提问。

---

## 18. 新版 PPT 新增/重组幻灯片演讲要点

> 本部分对应重写后的 `docs/FinalPre/main.tex`。新版 PPT 将协议栈独立成节，并新增“计算任务系统”“错误处理与容错”以及两条完整调用链幻灯片，总页数增加至约 45 页。以下给出新增/重组页面的讲解要点。

---

### 协议栈详解：链路帧 v2 线缆格式

**页面要点**：展示 19B 头部 + 最大 512B payload 的完整字节布局。

**讲解稿**：
> 链路帧 v2 的头部固定 19 字节。前两个字节是 magic `'M''H'`，用于 parser 在字节流中快速定位候选帧头。version 和 hdr_len 字段让未来扩展时可以前向兼容。type 字段分三类：链路维护 0x01~0x04、控制面 0x10~0x1F、数据面 0x80~0x83。src/dst 是 8 位 mesh 短地址，0x00 是主机，0xFF 表示未分配或广播。TTL 每转发一次递减，防止环路。seq/ack 预留可靠传输。最重要的是双层 CRC：头部 CRC 在收齐 19 字节后立即校验，payload CRC 在收齐整个帧后校验。

**预计讲解时间**：2 分钟。

---

### 协议栈详解：链路帧类型与解析器

**页面要点**：帧类型分类 + 流式解析器步骤。

**讲解稿**：
> 帧类型分成链路维护、控制面和数据面三类。数据面里 0x80 是 mini9P，0x81 是 RPC，0x82 是 Job，0x83 预留 Bulk。流式解析器不假设每次收到完整帧，它先找 magic，再收齐头部算 hdr_crc，接着按 payload_len 收 payload，最后算 payload_crc。任何一步出错都会丢弃当前候选，并尽量保留尾部 magic 前缀以快速恢复。

**预计讲解时间**：1.5 分钟。

---

### 协议栈详解：控制面协议（注册与地址分配）

**页面要点**：NODE_REGISTER 24B、ADDR_ASSIGN 28B 的字节布局。

**讲解稿**：
> 节点注册请求携带 96 位 UID、boot_id、能力标志和 upstream_port。UID 是 STM32 硬件唯一 ID，boot_id 是本次启动生成的随机值，两者一起保证节点重启后旧状态可被清理。地址分配响应携带 UID、boot_id、lease_epoch、lease_ms、分配到的 addr 和 flags。lease_epoch 用于区分不同轮次的租约，避免过期 ACK 被误用。

**预计讲解时间**：2 分钟。

---

### 协议栈详解：控制面协议（租约、链路与路由）

**页面要点**：LEASE_RENEW 24B、LINK_STATE 24B、ROUTE_UPDATE 12B。

**讲解稿**：
> 租约续期报文携带 UID、boot_id、lease_epoch 和 addr，coordinator 校验后返回 LEASE_ACK。链路状态报文携带 local_addr、local_port、peer_addr、peer_port、metric 以及对端 UID/boot_id。coordinator 收到一条链路状态后会生成两条 ROUTE_UPDATE：一条告诉 local 怎么到 peer，另一条告诉 peer 怎么到 local。节点按 route_version 更新本地表，旧版本直接忽略。

**预计讲解时间**：2 分钟。

---

### 协议栈详解：mini9P 协议帧

**页面要点**：mini9P 帧头 4B + payload，qid/stat 标志，消息类型。

**讲解稿**：
> mini9P 是精简版 9P，帧头只有 4 字节：version、type、tag 和 payload。消息类型包括 attach、walk、open、read、write、stat、clunk 以及对应的响应。每个对象用 qid 标识，qid 里 DIR、VIRTUAL、DEVICE、COMPUTE、READONLY 标志让主机知道对象类型。最大路径 255 字节，文件名 64 字节，错误文本 64 字节。

**预计讲解时间**：1.5 分钟。

---

### 协议栈详解：mini9P 请求/响应示例

**页面要点**：表格展示 T*/R* 消息的关键字段和用途。

**讲解稿**：
> 以 `cat /mcu1/sys/health` 为例，主机依次发送 TATTACH 建立 session、TWALK 定位 `/sys/health`、TOPEN 打开文件、TREAD 读取内容。节点返回 RATTACH、RWALK、ROPEN、RREAD。如果失败则返回 RERROR，带错误码和文本。这种请求-响应模式让远端文件访问看起来像本地文件操作。

**预计讲解时间**：1.5 分钟。

---

### 协议栈详解：MCU RPC 协议

**页面要点**：RPC 帧头 16B，最大 512B，service/method 各 31B。

**讲解稿**：
> MCU RPC 帧头 16 字节，包含 version、kind、flags、service_len、method_len、call_id、status、deadline_ms 和 payload_len。kind 包括 REQUEST、RESPONSE、CANCEL、STREAM_CHUNK、STREAM_END。flags 支持 ONEWAY 和 STREAM。stream chunk 的 status 字段复用为 chunk 序号，这样可以在不增加字段的情况下做顺序校验。完整性由外层 link frame 的 payload CRC 保证，所以 RPC 本身不需要额外 CRC。

**预计讲解时间**：1.5 分钟。

---

### 协议栈详解：Job 协议帧

**页面要点**：Job 帧头 20B，最大 512B，状态机。

**讲解稿**：
> Job 帧头 20 字节，包含 version、kind、state、kernel、request_id、status、job_id、progress_permille、result_len 和 payload_len。消息种类覆盖能力查询、提交、状态查询、结果查询和取消。状态机从 EMPTY 到 CREATED、QUEUED、ASSIGNED、RUNNING，最终到 DONE、FAILED、CANCELLED 或 LOST。progress_permille 用千分比表示进度，result_len 告诉主机结果有多大。

**预计讲解时间**：2 分钟。

---

### 协议栈详解：Host RPC 与 Leader 选举

**页面要点**：TCP/9909 + 4B CBOR 长度前缀，最大 payload 1024B，leader 按 (epoch, priority, host_uid) 选举。

**讲解稿**：
> 主机间通过 mDNS 发现 `_pwos._tcp` 服务，然后建立 TCP/9909 连接。每个 CBOR body 前面有 4 字节网络序长度前缀，方便精确收帧。最大 payload 1024 字节，最大帧 1280 字节。status 里专门有 NOT_LEADER，让客户端可以重试到 leader。leader 选举比较键是 epoch、priority、host_uid，角色包括 OBSERVER、FOLLOWER、LEADER，默认超时 15 秒。

**预计讲解时间**：1.5 分钟。

---

### 计算任务系统：为什么需要独立计算任务系统？

**页面要点**：MCU 资源受限，长计算不能阻塞通信。

**讲解稿**：
> STM32F4 主频 168MHz，RAM 192KB。矩阵乘、Mandelbrot、光线追踪一个 tile 可能耗时几百毫秒。如果放在 service_task 里执行，会阻塞 mini9P、RPC 和控制面。因此我们把计算拆成独立模块：低优先级 compute_task + 增量 worker，每次只推进一个最小工作单元，主动让出 CPU。

**预计讲解时间**：1.5 分钟。

---

### 计算任务系统：Job 状态机

**页面要点**：状态转换图 + 各状态含义。

**讲解稿**：
> Job 状态机从 EMPTY 槽位开始，提交后进入 QUEUED，被调度器选中后进入 RUNNING，完成后到 DONE。如果出错到 FAILED，被取消到 CANCELLED，节点丢失到 LOST。只有 DONE、FAILED、CANCELLED 是终止态，槽位可以被复用。

**预计讲解时间**：1 分钟。

---

### 计算任务系统：Kernel 目录与输入格式

**页面要点**：表格展示 5 个 kernel 的输入/输出格式和限制。

**讲解稿**：
> 目前实现了 5 个 kernel。HASH 做 FNV-1a 32 位哈希。VECTOR_ADD 是 16 位整数向量相加，最多 32 个元素。MATMUL 是小矩阵乘法，最多 8x8。MANDELBROT 用定点迭代生成分形图像，最大 16x16。RAYTRACE_TILE 用 smallpt 做光线追踪，请求 22 字节，最大 tile 16x7。每个 job 的 input 和 result 都有固定容量 320 字节。

**预计讲解时间**：2 分钟。

---

### 计算任务系统：compute_worker 数据结构

**页面要点**：展示 `pwos_compute_job_t` 结构体字段。

**讲解稿**：
> 这是 compute_worker 最核心的结构体。used 表示槽位占用；owner_addr 和 job_id 组成全局唯一键；sequence 用于 FIFO 调度；work_index 和 work_total 用于进度；accumulator 给 hash 用；input 和 result 是固定 320 字节的缓冲区；log 是 48 字节人类可读状态。

**预计讲解时间**：1.5 分钟。

---

### 计算任务系统：增量调度核心代码

**页面要点**：展示 `pwos_compute_worker_step()` 主循环。

**讲解稿**：
> `pwos_compute_worker_step` 每次只做一个工作单元。它先找有没有 RUNNING 的 job，没有则把最老的 QUEUED job 启动。然后按 kernel 调用对应的 step 函数。最后更新 progress_permille，如果 work_index 达到 work_total 就标记 DONE。整个函数持有 worker 锁保护状态，但执行时间很短，所以不会长时间阻塞。

**预计讲解时间**：2 分钟。

---

### 计算任务系统：Stepping 函数示例

**页面要点**：hash 和 matmul 的 stepping 代码。

**讲解稿**：
> hash kernel 每步处理一个字节，做 FNV-1a 的异或和乘。matmul kernel 每步计算结果矩阵的一个元素，内层对 inner 维度求和。每个 step 只处理一个单元，因此即使 matmul 的一个元素需要多次乘法，也不会长时间占用 CPU。

**预计讲解时间**：1.5 分钟。

---

### 计算任务系统：Raytrace Tile 分布式渲染示例

**页面要点**：渲染请求格式 + 执行流程 + LCD 显示。

**讲解稿**：
> Raytrace tile 请求 22 字节，包含 scene_id、tile 坐标、tile 尺寸、图像尺寸、采样数、最大深度、frame_id、seed 和 camera_phase。P4 上的 Lua 调度器把 240x320 的图像切成多个 tile，每个 tile 作为一个 Job 发到不同 MCU。MCU 用 smallpt 逐像素渲染，结果以 RGB565 格式返回。STM32F429 的 LCD 节点接收 tile 结果并显示到对应位置。

**预计讲解时间**：2 分钟。

---

### 计算任务系统：计算任务演示流程

**页面要点**：Shell 命令示例 + 演示截图。

**讲解稿**：
> 演示时可以先用 `job caps mcu1` 查询能力，然后 `job submit mcu1 matmul` 提交矩阵乘，用 `job status <id>` 看进度，用 `job result <id>` 取结果，用 `job cancel <id>` 取消。对于渲染演示，可以看到多个 MCU 同时渲染不同 tile，最后 LCD 上拼出完整画面。

**预计讲解时间**：1.5 分钟。

---

### 计算任务系统：任务槽位复用与取消

**页面要点**：槽位分配策略 + 取消机制。

**讲解稿**：
> 槽位复用优先找未使用的槽，满了之后选择最早完成的终止态 job，按 sequence 判断。这样保证最新提交的任务不会被立刻覆盖。取消时只要把状态从 QUEUED 或 RUNNING 改为 CANCELLED，并更新 queued/active 计数。后续 result 请求会返回 CANCELLED 状态。

**预计讲解时间**：1.5 分钟。

---

### 代码实现与数据流：调用链 cat /mcu1/sys/health

**页面要点**：分层表格展示主机侧和节点侧的调用链。

**讲解稿**：
> 从用户输入 `cat /mcu1/sys/health` 开始，WebShell 调用 cluster_vfs 解析路径，找到 route 和 addr，通过 session_manager 建立 mini9P session，发送 TATTACH/TWALK/TOPEN/TREAD。数据经 link frame DATA_MINI9P 发到 STM32，UART ISR 接收后经过 link_rx_task、port_manager、mesh_ctrl_task、node_control 投递到 service_task。service_runtime 调用 mini9P server，local_vfs 生成健康文本，响应沿原路返回。

**预计讲解时间**：2 分钟。

---

### 代码实现与数据流：调用链 job submit mcu1 matmul

**页面要点**：分层表格展示 Job 提交到结果返回的调用链。

**讲解稿**：
> 用户输入 `job submit mcu1 matmul`，host_shell 调用 job_command，再调用 job_manager_submit 分配 host_job_id，通过 session_manager 发送 DATA_JOB SUBMIT。STM32 的 service_runtime 交给 job_service，job_service 调用 compute_worker_submit 把任务入队。低优先级 compute_task 周期调用 `pwos_compute_worker_step` 增量执行。用户用 `job result <id>` 轮询时，主机发送 RESULT_REQUEST，节点返回 RESULT_RESPONSE。

**预计讲解时间**：2 分钟。

---

### 错误处理与容错：CRC 与噪声恢复

**页面要点**：双层 CRC + 重同步策略。

**讲解稿**：
> 链路层使用双层 CRC。头部 CRC 在收齐 19 字节后立即校验，如果头部损坏马上丢弃，不必等整个帧。payload CRC 在收齐后校验。解析器出错时会扫描缓冲区中的下一个 magic，保留未处理字节，尽快恢复同步。这种设计对 UART 噪声非常鲁棒。

**预计讲解时间**：1.5 分钟。

---

### 错误处理与容错：超时、重试与租约过期

**页面要点**：请求超时、租约过期、节点丢失检测。

**讲解稿**：
> RPC 和 pending 都带 deadline。超时后 session_manager 清理 pending 并返回 DEADLINE。节点在租约过半时发送 LEASE_RENEW，coordinator 超时未收到续期就释放地址。节点丢失检测结合 HELLO 超时、LEASE 超时和 LINK_STATE 缺失，标记 LOST 后清理路由和 pending。

**预计讲解时间**：1.5 分钟。

---

### 错误处理与容错：Job 取消与失败处理

**页面要点**：取消机制 + 失败来源。

**讲解稿**：
> Job 取消只对 QUEUED 或 RUNNING 有效，立即转 CANCELLED。失败来源包括输入校验失败 BAD_REQUEST、槽位满 BUSY、未知 kernel UNSUPPORTED、渲染请求解码失败 FAILED、结果未准备好 NOT_READY。所有状态都有对应的 Job 协议 status 返回给主机。

**预计讲解时间**：1.5 分钟。

---

*材料完成。祝答辩顺利！*
