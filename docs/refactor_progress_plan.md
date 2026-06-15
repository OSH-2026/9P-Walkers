# 9P-Walkers 重构进度计划

本文档是 `docs/refactor_architecture_plan.md` 的执行计划。它回答三个问题：

1. 每一步要做什么。
2. 每一步要测试什么。
3. 什么结果算通过，什么结果必须停下来修。

这份计划默认当前目标是先做出稳定的最小链路：

```text
ESP32-P4 host -- UART -- mcu1 -- UART -- mcu2
```

然后再扩展到 RPC、多主机和分布式计算。

## 0. 项目规则

重构期间先遵守这些规则，否则很容易继续制造不可解释的 bug。

- 不在旧 mesh 主循环里继续叠补丁，除非是为了保留可烧录、可回退的 baseline。
- 新协议、新状态机、新调度模型先在 PC 单测跑通，再接硬件。
- 控制面和数据面分开提交，不能在同一个提交里同时改 UART 驱动、路由、Mini9P、WebShell。
- 每个阶段必须有验收记录，建议放到 `docs/logs/refactor/YYYY-MM-DD-xxx.md`。
- 每个 PR 或合并点必须说明：改了什么状态所有权，新增了什么队列，新增了什么超时或重试策略。
- 从机端禁止阻塞式等待远端响应；等待必须通过 session/deadline 机制完成。
- 任何端口异常都只能影响这个端口，不能拖死本机 `/sys/health`。
- `EAGAIN` 只能表示确实等待到 deadline 后没有响应，不能作为立即失败的通用错误。

## 1. 总体里程碑

| 里程碑 | 目标 | 预计工作量 | 阶段门 |
|---|---|---:|---|
| M0 | 冻结当前 baseline，记录已知问题 | 0.5-1 天 | 当前代码可构建，可回退 |
| M1 | 新 link frame/parser 在 PC 上稳定 | 1-2 天 | 噪声、半帧、CRC、乱序测试通过 |
| M2 | 从机 FreeRTOS skeleton + UART DMA/IDLE 稳定 | 2-3 天 | 单板 30 分钟无阻塞、无 HardFault |
| M3 | 从机 port manager 自发现主机和邻居 | 2-3 天 | 任意端口接 host 都能识别 upstream |
| M4 | 控制面 address lease + topology + route 收敛 | 3-5 天 | `ESP32-mcu1-mcu2` 能稳定看到两个节点 |
| M5 | Mini9P 数据面重新接入 session manager | 3-4 天 | `ls`/`cat` 不再被拓扑变化拖死 |
| M6 | Host WebShell/LAN/API 稳定化 | 1-2 天 | 浏览器 shell 长连、并发、重连测试通过 |
| M7 | 系统观测接口和故障注入完善 | 1-2 天 | `/sys/*` 能定位端口、路由、任务、队列 |
| M8 | RPC alpha | 3-5 天 | 支持 unary/cancel/deadline |
| M9 | 分布式计算 alpha | 4-7 天 | job 下发、进度、结果、取消可用 |
| M10 | 多主机 alpha | 4-7 天 | leader/follower/observer 基本可用 |

M0-M6 是第一阶段必须完成的 MVP。M7 是调试质量保障。M8-M10 是高级能力，不应该在 M0-M6 稳定前混进来。

## 2. M0：冻结 baseline 和问题档案

### 2.1 要做什么

1. 从当前分支拉出重构分支，例如：

   ```bash
   git switch -c refactor/mesh-rtos-v2
   ```

2. 记录当前可构建状态：

   ```bash
   cd pwos-master-esp32p4
   idf.py build
   ```

   ```bash
   cd pwos-slave
   cmake --preset F407Debug
   cmake --build --preset F407Debug
   ```

3. 记录当前硬件 baseline：

   - 只接 `ESP32 -- mcu1`。
   - 接 `ESP32 -- mcu1 -- mcu2`，先不给 mcu2 供电。
   - 接 `ESP32 -- mcu1 -- mcu2`，给 mcu2 供电。
   - 记录 `mesh`、`ls /`、`cat /mcu1/sys/health`、`cat /mcu1/sys/routes`。

4. 建一个问题档案，至少记录：

   - mcu2 上电导致 mcu1 `cat` 变成 `EAGAIN` 或 `ENOENT`。
   - host monitor 反复 `bootstrap REGISTER -> ASSIGN /mcu1`。
   - `mesh` 显示 `bidi=0`、`to_port=255`。
   - WebShell 曾经输入一次后断连。
   - `ls /` 只能看到 `mcu1/`。

### 2.2 要测试什么

M0 不修 bug，只确认“旧系统坏在哪里”和“重构前还能不能回退”。

测试清单：

- ESP32-P4 固件能 build。
- STM32F407 固件能 build。
- WebShell 能通过 LAN 打开。
- 单 mcu1 情况下至少能偶尔访问 `/mcu1/sys/health`。
- mcu2 上电问题能被日志复现或记录为暂时非稳定复现。

### 2.3 通过标准

- `docs/logs/refactor/` 下有一份 baseline 记录。
- master 和 slave 至少各有一次成功构建记录。
- 当前已知问题被列出来，不再口头描述。

### 2.4 失败时怎么办

如果构建都失败，先只修构建，不碰 mesh 行为。构建修复必须单独提交。

## 3. M1：Link Frame 和 Parser

目标：把 UART 上的字节流变成一个独立、可测试、可复用的 link layer。它不懂 Mini9P、不懂路由、不懂 VFS。

建议新增目录：

```text
pwos-shared/link/
  pwos_link_frame.h
  pwos_link_frame.c
  pwos_link_parser.h
  pwos_link_parser.c
  pwos_link_crc.h
  pwos_link_crc.c
  tests/
```

### 3.1 要做什么

1. 定义统一 frame header。

   最低字段建议：

   ```text
   magic       2 bytes
   version     1 byte
   header_len  1 byte
   type        1 byte
   flags       1 byte
   src         1 byte
   dst         1 byte
   ttl         1 byte
   seq         2 bytes
   ack         2 bytes
   payload_len 2 bytes
   header_crc  2 bytes
   payload_crc 2 bytes
   payload     N bytes
   ```

2. 定义 frame type。

   第一批只实现这些：

   - `LINK_HELLO`
   - `LINK_HELLO_ACK`
   - `LINK_HEARTBEAT`
   - `LINK_ERROR`
   - `CTRL_NODE_REGISTER`
   - `CTRL_ADDR_ASSIGN`
   - `CTRL_LINK_STATE`
   - `CTRL_ROUTE_UPDATE`
   - `DATA_MINI9P`

3. Parser 必须是增量状态机。

   它要支持：

   - 一个字节一个字节喂入。
   - 一次喂入一整块 DMA buffer。
   - 遇到噪声能重新同步。
   - 半帧不会阻塞。
   - 长度异常直接丢弃当前候选帧。

4. 错误码标准化。

   建议新增统一错误码：

   ```text
   PWOS_OK
   PWOS_E_BAD_MAGIC
   PWOS_E_BAD_VERSION
   PWOS_E_BAD_LENGTH
   PWOS_E_BAD_CRC
   PWOS_E_NO_MEMORY
   PWOS_E_QUEUE_FULL
   PWOS_E_DEADLINE
   PWOS_E_LINK_DOWN
   PWOS_E_NOT_ROUTE
   ```

   对外命令显示时再映射成 `EAGAIN`、`ENOENT` 等，不要在底层到处传播裸 `-13`。

5. 写 golden frame。

   `tests/golden/` 中保存几组固定输入输出，避免后面协议改坏但没人知道。

### 3.2 要测试什么

PC 单测必须覆盖：

- 空输入。
- 单字节输入。
- 完整帧一次输入。
- 两个帧连续输入。
- 噪声 + 正常帧。
- 正常帧 + 噪声。
- 半帧分两次输入。
- payload 长度为 0。
- payload 长度为最大值。
- `payload_len` 比实际大。
- `payload_len` 比实际小。
- header CRC 错。
- payload CRC 错。
- version 不支持。
- magic 在 payload 中出现时不会错误同步。
- parser 处理 1 MB 随机噪声不会越界、死循环或内存增长。

### 3.3 验收命令

建议新建 CMake 测试工程后运行：

```bash
cmake -S pwos-shared/link/tests -B pwos-shared/link/tests/build
cmake --build pwos-shared/link/tests/build
pwos-shared/link/tests/build/pwos_link_frame_test
pwos-shared/link/tests/build/pwos_link_parser_test
```

### 3.4 通过标准

- Parser 不依赖 STM32 HAL 或 ESP-IDF。
- Parser 没有阻塞等待。
- 所有 PC 单测通过。
- 任意错误输入不会让 parser 状态不可恢复。
- 代码注释解释 frame 格式、CRC 范围和重新同步策略。

### 3.5 失败时怎么办

只修 `pwos-shared/link/`。不要同时改 STM32 或 ESP32 的业务代码。

## 4. M2：从机 FreeRTOS Skeleton 和 UART DMA

目标：从机从一开始就是 RTOS 架构，不再把 mesh、Mini9P、业务逻辑挤在 `while (1)` 里。

### 4.1 CubeMX 配置

只让 CubeMX 负责底层外设和 FreeRTOS 基础配置，不在 CubeMX 里配置复杂业务任务。

建议：

- 开启 FreeRTOS。
- 使用 CMSIS-RTOS v2 或原生 FreeRTOS 二选一，全项目统一。
- 如果 FreeRTOS 使用 SysTick，HAL timebase 改用一个普通 TIM，避免 SysTick 所有权冲突。
- UART 开启 DMA RX/TX。
- UART RX 使用 circular DMA + IDLE line interrupt。
- 每个 mesh UART 启用 NVIC。
- DMA IRQ 优先级不能高于 FreeRTOS 可管理中断上限。
- 不让调试日志和 mesh 二进制帧共用同一个 UART。

### 4.2 建议新增模块

```text
pwos-slave/User/rtos/
  pwos_tasks.h
  pwos_tasks.c
  pwos_queues.h
  pwos_queues.c

pwos-slave/User/drivers/
  uart_dma_port.h
  uart_dma_port.c
```

### 4.3 要做什么

1. 新建从机任务入口。

   最低任务集：

   | 任务 | 职责 |
   |---|---|
   | `link_rx_task[N]` | 从每个 UART DMA ring 取字节，喂 parser |
   | `link_tx_task` | 统一发帧，处理 TX queue |
   | `port_mgr_task` | 端口发现、角色、健康、隔离 |
   | `mesh_ctrl_task` | 地址租约、心跳、拓扑、路由控制面 |
   | `mesh_router_task` | 数据面转发，不处理业务 |
   | `session_task` | Mini9P/RPC 请求 deadline、重试、响应匹配 |
   | `resource_task` | 本地 `/sys`、VFS、设备资源 |
   | `app_task` | 板级主业务 |
   | `diag_task` | 周期性统计和调试快照 |

2. 所有任务通过 queue 通信。

   禁止：

   - ISR 里解析完整协议。
   - ISR 里 malloc。
   - task 之间直接改对方状态。
   - 数据面任务调用端口重置函数。

3. 实现 UART DMA driver 的最小能力。

   - `uart_port_start(port_id)`
   - `uart_port_stop(port_id)`
   - `uart_port_read_available(port_id, ...)`
   - `uart_port_write_frame(port_id, frame)`
   - `uart_port_get_stats(port_id)`

4. 每个任务提供 heartbeat。

   `/sys/tasks` 后续要能看到：

   - task name
   - stack high water mark
   - last heartbeat tick
   - queue depth
   - dropped messages

### 4.4 要测试什么

PC 层：

- 队列满时不会内存泄漏。
- `session_task` deadline 到期后能清理 pending 请求。
- `port_mgr_task` 不依赖具体 HAL。

STM32 单板：

- 上电后 30 秒内无 HardFault。
- 不接任何 UART，`diag_task` 仍正常 heartbeat。
- 接一个 USB-TTL loopback，发送噪声不会死机。
- DMA RX 在高频输入下不丢 parser 状态。
- TX queue 满时系统返回 `PWOS_E_QUEUE_FULL`，不阻塞。

### 4.5 验收命令

```bash
cd pwos-slave
cmake --preset F407Debug
cmake --build --preset F407Debug
```

烧录后观察串口或调试输出：

```text
[task] link_rx0 alive
[task] port_mgr alive
[task] mesh_ctrl alive
[task] resource alive
```

### 4.6 通过标准

- 没有业务逻辑留在主 `while (1)` 轮询里。
- 任意单个 UART 输入垃圾数据不会影响 `resource_task` heartbeat。
- 不接 host 时节点也能稳定运行。
- mcu2 供电或断电不能让 mcu1 的本地 `/sys/health` 卡死。

### 4.7 失败时怎么办

先停在单板 RTOS 和 DMA，不接 mesh。HardFault、栈溢出、DMA 越界问题没有解决前，不进入 M3。

## 5. M3：Port Manager 自发现

目标：从机自己分配串口资源，自己判断每个端口接的是 host、下游 node、普通 peer、噪声还是空口。

### 5.1 Port 状态机

建议状态：

```text
DISABLED
  -> PROBING
  -> LINK_UP
  -> HOST_CANDIDATE
  -> UPSTREAM
  -> DOWNSTREAM
  -> PEER
  -> SUSPECT
  -> QUARANTINED
```

每个端口独立维护：

- `port_id`
- `uart_instance`
- `role`
- `peer_uid`
- `peer_addr`
- `last_rx_tick`
- `last_tx_tick`
- `hello_seq`
- `heartbeat_miss`
- `crc_error_count`
- `parser_resync_count`
- `rx_dropped`
- `tx_dropped`
- `quarantine_until`

### 5.2 要做什么

1. board 层只提供候选 UART 列表。

   例如：

   ```c
   const pwos_port_desc_t pwos_board_ports[] = {
       { .id = 0, .name = "USART1", .huart = &huart1 },
       { .id = 1, .name = "USART2", .huart = &huart2 },
       { .id = 2, .name = "USART3", .huart = &huart3 },
       { .id = 3, .name = "UART4",  .huart = &huart4 },
       { .id = 4, .name = "USART6", .huart = &huart6 },
   };
   ```

   board 层不能写“这个就是上游口”。

2. 每个端口周期发送 `LINK_HELLO`。

   hello payload 至少包含：

   - protocol version
   - node uid
   - current addr，未分配时为 `0x00` 或 reserved unknown
   - role capability
   - boot_id
   - port_id
   - uptime_ms

3. 收到 `LINK_HELLO_ACK` 后更新端口角色。

   - 如果 peer capability 有 `ROLE_COORDINATOR`，进入 `HOST_CANDIDATE`。
   - 地址租约确认后进入 `UPSTREAM`。
   - 如果 peer 是普通 node，进入 `PEER` 或 `DOWNSTREAM`。

4. 端口隔离。

   如果某端口持续 CRC 错、协议版本错、heartbeat miss 或 TX timeout：

   - 只把这个端口设为 `SUSPECT`。
   - 超过阈值设为 `QUARANTINED`。
   - 一段时间后重新进入 `PROBING`。
   - 不能清空全局路由。
   - 不能重启整个 mesh。

### 5.3 要测试什么

PC 状态机测试：

- 空口：`PROBING` 周期发送 hello，不进入 UPSTREAM。
- host 接入：`PROBING -> HOST_CANDIDATE -> UPSTREAM`。
- node 接入：`PROBING -> PEER`。
- host 断开：`UPSTREAM -> SUSPECT -> PROBING`。
- 噪声口：`PROBING -> QUARANTINED -> PROBING`，其他端口不受影响。
- 两个 host 同时接入：都成为 `HOST_CANDIDATE`，交给 multi-host 策略选择，不崩。
- peer 重启后 boot_id 改变，旧 session 被清理。

硬件测试：

- host 接任意一个 UART，mcu1 都能识别 upstream。
- mcu1 的另一个 UART 接 mcu2，mcu1 能看到 peer。
- mcu2 不供电时，mcu1 upstream 仍稳定。
- mcu2 反复供电 20 次，mcu1 upstream 不重置。
- 拔掉 mcu2 后，mcu1 `/sys/health` 仍可读。

### 5.4 通过标准

- `cat /mcu1/sys/ports` 能看到所有端口状态。
- mcu2 端口异常不影响 mcu1 upstream。
- host monitor 不应反复看到同一个 mcu1 bootstrap register，除非 mcu1 真的重启或 lease 过期。
- `bidi=0` 必须能从 `/sys/links` 中解释清楚：是还没确认双向，还是端口被隔离。

### 5.5 失败时怎么办

如果 mcu2 上电仍会导致 mcu1 upstream 消失，先看 `/sys/ports`：

- 如果 upstream 端口也进入 `SUSPECT`，优先查电气串扰、供电、共地和 DMA buffer 越界。
- 如果只有 mcu2 端口异常，但 upstream 被删除，是 port manager 或 mesh control 的所有权错误。
- 如果 task heartbeat 停止，是 RTOS 调度或阻塞问题。

## 6. M4：Mesh Control Plane

目标：地址分配、拓扑发现、路由传播从数据面里拆出来。控制面必须在普通 `cat` 卡住时仍能工作。

### 6.1 建议新增模块

```text
pwos-shared/mesh2/
  mesh_identity.h/.c
  mesh_control_frame.h/.c
  mesh_topology.h/.c
  mesh_route.h/.c
  mesh_lease.h/.c
  tests/

pwos-master-esp32p4/mesh2/
  host_coordinator.h/.c
  host_topology_db.h/.c

pwos-slave/User/mesh2/
  node_control.h/.c
  node_forwarder.h/.c
```

可以先以 `mesh2` 并行开发，避免把旧 mesh 直接改烂。等 M5 稳定后再删除或替换旧模块。

### 6.2 要做什么

1. 定义身份模型。

   - `node_uid`：硬件唯一或烧录时生成，长期稳定。
   - `boot_id`：每次启动变化，用来识别重启。
   - `addr`：运行时短地址，由 coordinator 租约分配。
   - `lease_epoch`：地址租约版本。

2. Host coordinator 分配地址。

   控制流程：

   ```text
   node -> host: CTRL_NODE_REGISTER(uid, boot_id, caps, upstream_port)
   host -> node: CTRL_ADDR_ASSIGN(addr, lease_epoch, lease_ms)
   node -> host: CTRL_LEASE_ACK(addr, lease_epoch)
   ```

3. 节点周期续约。

   - lease 未确认前不能承载数据面请求。
   - lease 过期后进入 limited 模式，只保留 hello/register。
   - boot_id 改变时 host 清理旧 session。

4. 拓扑更新。

   每个节点上报本地 link state：

   ```text
   local addr
   local uid
   port_id
   peer addr or uid
   peer caps
   metric
   link flags
   last_seen
   ```

5. 路由计算只在 host 或明确的 owner 中完成。

   第一阶段建议 host owns global topology：

   - host 计算 route table。
   - host 下发每个节点需要的 next-hop。
   - node 只维护本地 forwarding table。

6. 控制面消息优先级高于数据面。

   TX queue 至少分两级：

   - high priority：hello、heartbeat、lease、route。
   - normal priority：Mini9P/RPC data。

### 6.3 要测试什么

PC 模拟测试：

- 单节点注册。
- 两节点链式注册。
- mcu1 先上线，mcu2 后上线。
- mcu2 先启动但没有 host，接入 mcu1 后通过 mcu1 注册。
- mcu2 重启，host 保持 mcu1 地址不变，只更新 mcu2 boot_id。
- mcu1 重启，host 删除经 mcu1 到 mcu2 的旧路由。
- 重复 REGISTER 不应重复创建 `/mcu1`。
- REGISTER 丢包后重试不产生重复节点。
- 地址租约过期后节点进入 limited 模式。
- route update 乱序时旧 epoch 不覆盖新 route。

硬件测试：

```text
ESP32 -- mcu1
```

- `mesh` 显示 host local。
- `ls /` 显示 `mcu1/`。
- `cat /mcu1/sys/health` 稳定。
- 复位 mcu1 后，重新注册但不会出现重复 `/mcu1`。

```text
ESP32 -- mcu1 -- mcu2
```

- `ls /` 显示 `mcu1/` 和 `mcu2/`。
- `mesh` 能看到 `0x00 -> 0x11` 和 `0x00 -> 0x12` 或等价路由。
- mcu2 复位时，mcu1 保持在线。
- mcu2 断电时，mcu1 保持在线。

### 6.4 通过标准

- `mesh` 输出能区分 node online、link online、route installed、session attached。
- 不能再出现“nodes 显示 online，但 route 已经没了”的不可解释状态。
- 控制面 30 分钟压力测试无重复挂载、无地址漂移。
- mcu2 的 link down 不会删除 mcu1 的 addr lease。

### 6.5 失败时怎么办

不要直接改 Mini9P。先看控制面：

- lease 是否存在。
- route epoch 是否正确。
- upstream port 是否稳定。
- link state 是否被错误广播。
- host topology DB 是否把 transient link 当成 node down。

## 7. M5：Mini9P 数据面和 Session Manager

目标：把 `ls`、`cat`、`read`、`write` 放在可控 session 里，解决“一个请求失败拖死全局”的问题。

### 7.1 要做什么

1. 引入 session manager。

   每个目的节点维护：

   - session id
   - peer addr
   - peer boot_id
   - max inflight
   - next tag
   - pending request table
   - deadline
   - retry policy
   - last error

2. Mini9P client 不直接操作 transport。

   调用链应变成：

   ```text
   cluster_vfs
     -> mini9p_client
       -> session_manager_send(DATA_MINI9P)
         -> mesh_router
           -> link_tx_queue
   ```

3. 超时策略明确。

   建议：

   - 本地 `/sys/*` 读：不等待链路，必须立即返回。
   - 一跳 Mini9P 默认 deadline：500-800 ms。
   - 两跳 Mini9P 默认 deadline：1000-1500 ms。
   - deadline 到期才返回 `PWOS_E_DEADLINE`，上层再映射到 `EAGAIN` 或更准确的错误。
   - 立即发现无 route 时返回 `PWOS_E_NOT_ROUTE`，不要伪装成 `EAGAIN`。

4. 控制面和数据面解耦。

   - Mini9P 请求等待响应时，heartbeat 继续发。
   - Mini9P 请求超时不能删除 port。
   - port down 可以通知 session fail，但不能反向重启控制面。

5. Fid 生命周期检查。

   - node boot_id 改变，清理该 node 所有 fid。
   - session reset，清理 pending request。
   - `Twalk/Topen/Tread/Tclunk` 失败路径必须释放本地资源。

### 7.2 要测试什么

PC 单测：

- 正常 `attach -> walk -> open -> read -> clunk`。
- `Twalk` 超时后 pending request 清理。
- `Topen` 失败后不会泄漏 fid。
- 响应 tag 不匹配被丢弃。
- 旧 boot_id 的响应被丢弃。
- route down 时新请求立即失败为 `PWOS_E_NOT_ROUTE`。
- queue full 时返回 `PWOS_E_QUEUE_FULL`，不会阻塞。
- control frame 在 data queue 堵塞时仍可发送。

Host VFS 测试：

```bash
cmake -S pwos-master-esp32p4/vfs_bridge/test -B pwos-master-esp32p4/vfs_bridge/test/build
cmake --build pwos-master-esp32p4/vfs_bridge/test/build
pwos-master-esp32p4/vfs_bridge/test/build/cluster_vfs_test
pwos-master-esp32p4/vfs_bridge/test/build/mesh_host_service_test
pwos-master-esp32p4/vfs_bridge/test/build/mesh_host_runtime_test
```

硬件 smoke test：

```text
ls /
cat /mcu1/sys/health
cat /mcu1/sys/ports
cat /mcu1/sys/routes
cat /mcu2/sys/health
cat /mcu2/sys/ports
cat /mcu2/sys/routes
```

故障测试：

- 执行 `cat /mcu1/sys/health` 循环时给 mcu2 上电。
- 执行 `cat /mcu1/sys/health` 循环时拔掉 mcu2。
- 执行 `cat /mcu2/sys/health` 时复位 mcu2。
- 执行 100 次 `ls /`。
- 同时开两个 WebShell 客户端执行 `cat`。

### 7.3 通过标准

- mcu2 上电或断电时，`cat /mcu1/sys/health` 仍稳定。
- `cat /mcu2/sys/health` 如果失败，错误能说明是 no route、deadline、link down 还是 node reboot。
- 不再出现“很快就 EAGAIN，但根本没等到 deadline”的情况。
- WebShell 不因单次 Mini9P 超时断线。
- 100 次 `ls /` 不丢 mcu1。

### 7.4 失败时怎么办

先确认错误类型：

- 立即失败：检查 route/session 前置检查。
- 等到 deadline 失败：检查下游 link/forwarding。
- WebShell 断线：检查 shell command handler 是否阻塞 HTTP server task 或触发 socket close。
- mcu1 也失败：检查端口隔离和控制面所有权，不要先怀疑 Mini9P server。

## 8. M6：Host WebShell、LAN 和 API 稳定化

目标：浏览器只是入口，不应该因为底层请求超时而断连；LAN host 也不应该和 mesh host 状态绑死。

### 8.1 要做什么

1. WebShell command 执行和 websocket IO 分离。

   - websocket task 只负责收发。
   - shell command 丢到 command queue。
   - 执行结果异步写回 websocket。
   - 每个客户端有独立 session。

2. Shell 命令超时。

   - 每条命令有 command deadline。
   - 超时返回错误文本，但 websocket 保持连接。
   - 长命令支持 progress 或 partial output。

3. LAN 状态观测。

   - `wifi status` 或 `net status` 显示 IP、mDNS、HTTP server、WebSocket client 数。
   - LAN 断开重连不重置 mesh runtime。

4. HTTP API 最小化。

   建议提供：

   - `GET /api/mesh`
   - `GET /api/nodes`
   - `GET /api/node/<name>/sys/health`
   - `GET /api/logs`

   WebShell 可以先继续用 websocket 文本协议。

### 8.2 要测试什么

浏览器测试：

- 打开 `http://pwos.local/`。
- 输入 `heap`，不断线。
- 输入 `wifi status` 或 `net status`，不断线。
- 输入 `cat /mcu2/sys/health`，即使超时也不断线。
- 刷新页面后旧 session 被清理。
- 两个浏览器同时打开，互不抢输出。

ESP32 测试：

- LAN DHCP 成功。
- mDNS 成功或失败都有明确日志。
- HTTP server task stack 不溢出。
- WebSocket client 断开后资源释放。

### 8.3 通过标准

- 30 分钟 WebShell 空闲不断线，或断线后自动重连且不影响 mesh。
- 单条 shell 命令失败不会关闭 websocket。
- 两个客户端同时执行命令时输出不串线。
- mesh 控制面日志和 WebShell 输出不互相污染。

## 9. M7：系统观测和故障注入

目标：以后出问题时先看状态，而不是靠猜。

### 9.1 必须实现的 `/sys` 接口

每个从机：

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
/sys/build
```

Host：

```text
/host/sys/health
/host/sys/links
/host/sys/topology
/host/sys/routes
/host/sys/sessions
/host/sys/web
/host/sys/log
```

### 9.2 每个接口最低内容

`/sys/health`：

- ok/fail
- uptime
- free heap
- boot_id
- addr
- role

`/sys/ports`：

- port id/name
- role/state
- peer uid/addr
- rx/tx count
- crc errors
- heartbeat miss
- quarantine until

`/sys/tasks`：

- task name
- priority
- stack high water mark
- last heartbeat
- queue depth

`/sys/routes`：

- dst
- next hop
- port
- metric
- epoch
- owner

`/sys/sessions`：

- peer
- boot_id
- inflight
- next tag
- last error
- deadline count

### 9.3 故障注入命令

建议 shell 支持调试命令，默认只在 debug build 开启：

```text
fault drop port <id> <percent>
fault delay port <id> <ms>
fault corrupt port <id> <percent>
fault down port <id>
fault recover port <id>
fault reboot-self
```

### 9.4 要测试什么

- 注入 10% 丢包，control plane 能恢复。
- 注入 100 ms 延迟，Mini9P deadline 行为正确。
- 注入 CRC 错，端口进入 `SUSPECT/QUARANTINED`。
- 恢复端口后重新 hello/register。
- 故障只影响目标端口。

### 9.5 通过标准

- 每次异常都有计数器可查。
- `/sys/log` 中有最近 N 条关键事件。
- 故障注入后恢复不需要整板重启。

## 10. M8：RPC Alpha

目标：在 mesh 数据面上增加 RPC，但不破坏 Mini9P。RPC 是新 data type，不是塞进 shell 文本命令里的特殊 case。

### 10.1 要做什么

1. 定义 RPC frame。

   字段建议：

   ```text
   rpc_version
   call_id
   method_id or method_name
   flags
   deadline_ms
   payload_format
   payload_len
   payload
   ```

2. RPC 支持四类调用：

   - unary request/response
   - server streaming
   - fire-and-forget
   - cancel

3. 从机注册 RPC service。

   例如：

   ```text
   /mcu1.rpc.sys.ping
   /mcu1.rpc.compute.hash
   /mcu1.rpc.device.read_adc
   ```

4. RPC 与 Mini9P 共用 session manager，但 pending table 分类型。

5. Shell 最小命令：

   ```text
   rpc /mcu1.sys.ping '{}'
   rpc /mcu2.compute.hash '{"data":"abcd"}'
   rpc-cancel <call_id>
   ```

### 10.2 要测试什么

PC 单测：

- unary success。
- unknown method。
- payload too large。
- deadline exceeded。
- cancel before start。
- cancel while running。
- response from old boot_id 被丢弃。
- 10 个并发 RPC 不影响 Mini9P `cat /sys/health`。

硬件测试：

- `rpc /mcu1.sys.ping '{}'`。
- `rpc /mcu2.sys.ping '{}'`。
- mcu2 RPC 执行中复位 mcu2。
- RPC deadline 后 WebShell 不断线。

### 10.3 通过标准

- RPC 错误能区分 method not found、deadline、cancelled、node down。
- RPC 不能阻塞 control plane。
- RPC 不能阻塞本地 `/sys/health`。

## 11. M9：分布式计算 Alpha

目标：把“计算任务”抽象成 job，而不是直接让 shell 长时间等待某个 RPC。

### 11.1 要做什么

1. Host 侧新增 job manager。

   ```text
   pwos-master-esp32p4/jobs/
     job_manager.h/.c
     job_scheduler.h/.c
     job_store.h/.c
   ```

2. 从机侧新增 compute worker。

   ```text
   pwos-slave/User/compute/
     compute_worker.h/.c
     compute_registry.h/.c
   ```

3. 定义 job 生命周期。

   ```text
   CREATED
   QUEUED
   ASSIGNED
   RUNNING
   STREAMING_RESULT
   DONE
   FAILED
   CANCELLED
   LOST
   ```

4. 节点能力上报。

   capability 至少包含：

   - cpu class
   - free memory
   - supported kernels
   - max job payload
   - current load

5. 最小计算任务。

   第一批不要上复杂 AI workload，先做可验证任务：

   - hash
   - vector add
   - small matrix multiply
   - Mandelbrot tile

### 11.2 要测试什么

PC 模拟：

- job 分配给空闲节点。
- 节点忙时排队。
- job running 时节点断线，job 变 `LOST` 或重试。
- cancel job。
- job result 分块传输。
- job progress 更新。

硬件测试：

- `submit hash.json` 到 mcu1。
- `submit matmul.json` 到 mcu2。
- mcu2 运行 job 时 `cat /mcu1/sys/health` 不受影响。
- mcu2 运行 job 时复位 mcu2，host 能标记 job lost。

### 11.3 通过标准

- job 不通过 shell 长阻塞实现。
- job 状态可查询。
- job failure 不影响 mesh control plane。
- compute worker 有独立任务和栈，不占用 mesh task。

## 12. M10：多主机 Alpha

目标：允许多个 host 存在，但先只做基础一致性，不做复杂分布式共识。

### 12.1 角色

```text
LEADER    拥有地址分配和全局 route 权限
FOLLOWER  接收 leader 同步，可在 leader 消失后候选接管
OBSERVER  只读观察，不下发 route
CLIENT    只发起请求，不参与管理
```

### 12.2 要做什么

1. Host identity。

   - `host_uid`
   - `cluster_id`
   - `host_epoch`
   - `priority`

2. Host advertise。

   host 周期广播：

   - role
   - epoch
   - priority
   - topology digest

3. 冲突处理。

   第一版规则可以简单：

   - 同一 cluster 中 highest `(epoch, priority, host_uid)` 为 leader。
   - follower 不分配地址。
   - observer 不写 route。
   - 节点只接受当前 leader 的 addr assign 和 route update。

4. Topology sync。

   leader 向 follower 同步：

   - node table
   - link table
   - route table
   - lease table

### 12.3 要测试什么

PC 模拟：

- 单 leader。
- leader + observer。
- 两个 leader 冲突，收敛到一个。
- leader 断开，follower 升级。
- 旧 leader 回来，epoch 低时不能覆盖新 leader。
- 节点拒绝非 leader 的 route update。

硬件测试：

- ESP32-P4 host + PC host emulator 同时接入。
- PC host observer 能看 topology。
- observer 不能破坏 ESP32 leader 的 route。

### 12.4 通过标准

- 多 host 不会导致重复地址。
- 节点能报告当前 leader。
- leader 切换不会让所有节点永久失联。

## 13. 第一阶段 MVP 的具体每日安排

这是建议节奏，可以按人力调整，但阶段顺序不要乱。

### Day 1：baseline 和设计冻结

要做：

- 完成 M0 baseline。
- 确认 `docs/refactor_architecture_plan.md` 和本文档作为重构依据。
- 决定新模块命名：`link/mesh2` 还是直接替换旧 `mesh/`。
- 决定 FreeRTOS API：CMSIS-RTOS v2 或原生 FreeRTOS。

要测：

- master build。
- slave F407Debug build。
- 旧系统硬件行为记录。

产出：

- baseline log。
- 重构分支。
- 第一批接口头文件草案。

### Day 2-3：link parser

要做：

- `pwos-shared/link`。
- frame encode/decode。
- parser 增量状态机。
- CRC。
- golden frames。

要测：

- parser 单测。
- 噪声/半帧/CRC/随机输入。

产出：

- M1 通过。

### Day 4-6：RTOS skeleton + DMA

要做：

- CubeMX 开 FreeRTOS、DMA、IDLE。
- 新建任务和队列。
- UART DMA driver。
- 单板 `/sys/health` 本地资源先跑通。

要测：

- 单板无 host 运行 30 分钟。
- UART 噪声输入。
- TX queue full。
- task heartbeat。

产出：

- M2 通过。

### Day 7-9：port manager

要做：

- port FSM。
- hello/hello_ack。
- port quarantine。
- `/sys/ports`。

要测：

- 任意 UART 接 host。
- mcu2 上电/断电不影响 mcu1 本地状态。
- 端口噪声隔离。

产出：

- M3 通过。

### Day 10-14：control plane

要做：

- node register。
- addr assign。
- lease renew。
- link state。
- host topology DB。
- route update。

要测：

- PC 拓扑模拟。
- `ESP32 -- mcu1`。
- `ESP32 -- mcu1 -- mcu2`。
- 重复 register。
- mcu2 reboot。

产出：

- M4 通过。

### Day 15-18：Mini9P session

要做：

- session manager。
- Mini9P adapter。
- route/session error mapping。
- cluster VFS 适配。

要测：

- Host VFS PC tests。
- `ls /` 100 次。
- `cat /mcu1/sys/health` while mcu2 power cycle。
- `cat /mcu2/sys/health` timeout/recovery。

产出：

- M5 通过。

### Day 19-20：WebShell 和观测

要做：

- websocket IO 和 command 执行解耦。
- shell command deadline。
- `/sys/tasks`、`/sys/routes`、`/sys/sessions`。
- 故障注入最小命令。

要测：

- 两个浏览器并发。
- 命令超时不断线。
- 30 分钟 idle。
- fault drop/delay/corrupt。

产出：

- M6/M7 基础通过。

## 14. MVP 验收脚本

MVP 完成时，至少手动跑完下面这组测试。

### 14.1 构建

```bash
cd pwos-master-esp32p4
idf.py build
```

```bash
cd pwos-slave
cmake --preset F407Debug
cmake --build --preset F407Debug
```

```bash
cmake -S pwos-master-esp32p4/vfs_bridge/test -B pwos-master-esp32p4/vfs_bridge/test/build
cmake --build pwos-master-esp32p4/vfs_bridge/test/build
pwos-master-esp32p4/vfs_bridge/test/build/cluster_vfs_test
pwos-master-esp32p4/vfs_bridge/test/build/mesh_host_service_test
pwos-master-esp32p4/vfs_bridge/test/build/mesh_host_runtime_test
```

### 14.2 单 mcu1

拓扑：

```text
ESP32 -- mcu1
```

命令：

```text
mesh
ls /
cat /mcu1/sys/health
cat /mcu1/sys/ports
cat /mcu1/sys/tasks
cat /mcu1/sys/routes
```

通过标准：

- `ls /` 显示 `mcu1/`。
- `cat /mcu1/sys/health` 连续 100 次成功。
- `mesh` 中 mcu1 route 稳定。
- 没有重复 register 日志风暴。

### 14.3 链式双 mcu

拓扑：

```text
ESP32 -- mcu1 -- mcu2
```

命令：

```text
mesh
ls /
cat /mcu1/sys/health
cat /mcu2/sys/health
cat /mcu1/sys/ports
cat /mcu2/sys/ports
cat /mcu1/sys/routes
cat /mcu2/sys/routes
```

通过标准：

- `ls /` 显示 `mcu1/` 和 `mcu2/`。
- mcu1 和 mcu2 都有不同 addr。
- mcu1 upstream 是 host。
- mcu1 有一个 downstream/peer 指向 mcu2。
- mcu2 upstream 是 mcu1。
- 任意一个 `cat` 失败时能给出准确错误类型。

### 14.4 mcu2 电源扰动

步骤：

1. 保持 `ESP32 -- mcu1` 稳定。
2. 每秒执行一次：

   ```text
   cat /mcu1/sys/health
   ```

3. 给 mcu2 上电。
4. 观察 60 秒。
5. 给 mcu2 断电。
6. 观察 60 秒。
7. 重复 20 次。

通过标准：

- mcu1 health 不能因为 mcu2 上电而持续失败。
- mcu1 upstream port 不能重置。
- mcu2 端口可以进入 `SUSPECT/QUARANTINED/PROBING`，但不能影响 mcu1 本地 resource。
- host 不出现 mcu1 重复注册风暴。

### 14.5 WebShell

步骤：

1. 浏览器打开 `http://pwos.local/` 或 host IP。
2. 连续输入：

   ```text
   heap
   net status
   mesh
   ls /
   cat /mcu1/sys/health
   cat /mcu2/sys/health
   ```

3. 第二个浏览器同时打开并执行同样命令。
4. 让 mcu2 复位。
5. 再执行 `cat /mcu1/sys/health`。

通过标准：

- WebShell 不断线。
- 两个客户端输出不串。
- mcu2 失败不影响 mcu1。
- 错误文本明确。

## 15. 不同错误的定位路径

### 15.1 `ls /` 看不到 mcu2

按顺序查：

1. `cat /mcu1/sys/ports`：mcu1 是否看到 mcu2 端口。
2. `cat /mcu1/sys/links`：link 是否 bidirectional。
3. `cat /mcu2/sys/health`：如果不可达，查 mcu2 是否拿到 addr。
4. host `/host/sys/topology`：host 是否收到 mcu1 上报的 link state。
5. host `/host/sys/routes`：是否给 mcu2 安装 route。

不要直接改 `ls`。

### 15.2 `cat /mcu1/sys/health` 在 mcu2 上电后失败

这是 P0 级别 bug。

按顺序查：

1. mcu1 task heartbeat 是否停。
2. mcu1 upstream port 是否变 `SUSPECT`。
3. mcu1 mcu2 port 是否抢占全局锁。
4. control plane 是否误删 mcu1 route。
5. session manager 是否把 mcu2 错误广播到 mcu1 session。

通过架构要求，这个问题必须被隔离在 mcu2 port 或 mcu2 session。

### 15.3 立即返回 `EAGAIN`

立即 `EAGAIN` 通常是错误映射问题。

按顺序查：

1. 请求是否真的进入 pending table。
2. deadline 是否被设置成 0 或已过期。
3. 是否 route 不存在却被映射成 `EAGAIN`。
4. 是否 queue full 被映射成 `EAGAIN`。
5. 是否 session reset 后旧请求立即失败。

正确行为：

- no route：`PWOS_E_NOT_ROUTE`。
- queue full：`PWOS_E_QUEUE_FULL`。
- link down：`PWOS_E_LINK_DOWN`。
- 等到 deadline：`PWOS_E_DEADLINE`，上层可显示 `EAGAIN`。

### 15.4 反复 REGISTER

按顺序查：

1. 节点是否真实重启：看 boot_id。
2. lease 是否过短或续约失败。
3. host 是否没有保存 uid -> node name 映射。
4. port manager 是否不断把 upstream 设为 down/up。
5. host 是否把重复 register 当新节点。

重复 REGISTER 不是一定错误，但重复创建节点、重复 attach、重复清空 session 是错误。

## 16. 团队分工建议

如果多人同时做，建议按边界分工：

| 人 | 负责范围 | 禁止触碰 |
|---|---|---|
| A | `pwos-shared/link` 和 parser 单测 | 不改 WebShell/VFS |
| B | STM32 FreeRTOS、DMA、port manager | 不改 host topology |
| C | host coordinator、topology、route | 不改 UART HAL |
| D | Mini9P session、cluster VFS adapter | 不改 port FSM |
| E | WebShell、HTTP API、观测页面 | 不改 mesh core |

每个人的提交必须带：

- 修改原因。
- 状态所有权说明。
- 测试命令和结果。
- 对其他模块的接口影响。

## 17. 第一批代码落地顺序

建议按这个顺序开 PR 或提交：

1. `docs`: 架构蓝图和进度计划。
2. `shared-link`: frame/parser/crc + PC tests。
3. `slave-rtos`: FreeRTOS skeleton，不接真实 mesh。
4. `slave-uart-dma`: UART DMA driver + loopback/noise test。
5. `slave-port-manager`: port FSM + `/sys/ports`。
6. `shared-mesh-control`: identity/lease/topology/route PC tests。
7. `host-coordinator`: host topology DB + addr assign。
8. `node-control`: node register/lease/link state。
9. `mesh-forwarder`: data frame forwarding。
10. `mini9p-session`: Mini9P adapter + session manager。
11. `webshell-async`: WebShell command queue + deadline。
12. `observability`: `/sys/tasks/routes/sessions/log`。

任何一个顺序失败，都不要跳到后面的业务功能。

## 18. 最终 MVP Definition of Done

MVP 只算 M0-M7，不包含 RPC、多主机、分布式计算。完成标准：

- ESP32-P4 host 通过 LAN 提供 WebShell。
- 从机使用 FreeRTOS，mesh 和业务不在同一个主循环。
- 从机端口自发现，不依赖固定“上游/下游”配置。
- `ESP32 -- mcu1 -- mcu2` 稳定显示两个节点。
- mcu2 上电、断电、复位不会让 mcu1 不可访问。
- `ls /`、`cat /mcu1/sys/health`、`cat /mcu2/sys/health` 有明确成功或失败语义。
- WebShell 不因底层命令失败而断线。
- PC 单测覆盖 link parser、port FSM、route、session。
- `/sys/ports`、`/sys/routes`、`/sys/sessions` 能解释现场状态。
- 关键代码有说明“为什么这么设计”的注释，而不是只解释语法。
