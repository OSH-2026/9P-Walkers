# 成员 C 答辩准备：主机数据面与会话层

本文面向“成员 C：主机数据面与会话层”。你的核心任务不是讲底层 UART 或 STM32 任务，而是把用户可见的数据访问如何经过主机侧抽象、会话管理、mini9P/RPC/Job 协议，最终变成发往 MCU 的 DATA_* 帧讲清楚。

## 一句话定位

我负责主机数据面：把 `/mcuN/...`、RPC 调用和 Job 命令统一收敛到 `session_manager`，由它按 `(data_type, src_addr, wire_tag)` 匹配响应，并用 `boot_id/generation` 防止节点重启或地址复用导致旧请求、旧 fd、旧 job 污染新节点。

## 必背数字

| 项目 | 值 | 代码位置 |
|---|---:|---|
| session 数组上限 | 32 | `pwos-master-esp32p4/host_sessions/session_manager.h:14` |
| pending 数组上限 | 8 | `pwos-master-esp32p4/host_sessions/session_manager.h:16` |
| session 默认 deadline | 1000ms | `pwos-master-esp32p4/host_sessions/session_manager.h:18` |
| cluster_vfs route 上限 | 32 | `pwos-master-esp32p4/host_api/cluster_vfs.h:16` |
| cluster_vfs open fd 上限 | 16 | `pwos-master-esp32p4/host_api/cluster_vfs.h:18` |
| mini9P 帧开销 | 10 字节 | `pwos-shared/mini9p/mini9p_protocol.h:15` |
| mini9P client buffer | 512 字节 | `pwos-shared/mini9p/mini9p_client.h` |
| RPC header | 16 字节 | `pwos-shared/rpc/pwos_rpc_protocol.h` |
| Job header | 20 字节 | `pwos-shared/job/pwos_job_protocol.h` |
| Job 本地槽位 | 16 | `pwos-master-esp32p4/host_jobs/job_manager.h:14` |

## 总体调用链

### 读取 `/mcu1/sys/health`

1. WebShell/主机 API 调 `pwos_cluster_vfs_read_path("/mcu1/sys/health")`。
2. `cluster_vfs` 在 route 表里匹配 `mcu1`，把远端路径变成 `/sys/health`。
3. 如果 route 还没 attach，先调用 `pwos_session_manager_attach()`，完成 mini9P `TATTACH/RATTACH`。
4. `cluster_vfs_open()` 调 `m9p_client_open_path()`，内部依次 `TWALK` 到 `/sys/health`，再 `TOPEN`。
5. `cluster_vfs_read()` 循环发 `TREAD`，直到返回 0 字节 EOF 或缓冲区满。
6. `cluster_vfs_close()` 发 `TCLUNK`，释放远端 fid。
7. 每个 mini9P 请求都会进入 `session_transport()`，再由 session_manager 分配 wire_tag、创建 pending、发送 DATA_MINI9P。

关键代码：

| 责任 | 代码 |
|---|---|
| 解析 `/mcuN` 并映射远端路径 | `pwos-master-esp32p4/host_api/cluster_vfs.c:434` |
| 确保 mini9P attach | `pwos-master-esp32p4/host_api/cluster_vfs.c:384` |
| open/read/close 主路径 | `cluster_vfs.c:625`, `cluster_vfs.c:727`, `cluster_vfs.c:895` |
| mini9P transport 接入 pending | `pwos-master-esp32p4/host_sessions/session_manager.c:313` |
| mini9P tag 恢复 | `session_manager.c:414` |

### RPC 调用

1. `pwos_rpc_client_call()` 用 `pwos_rpc_encode()` 生成 REQUEST 帧，`call_id` 先填 0。
2. 交给 `pwos_session_manager_request_data()`，指定 `data_type = DATA_RPC`。
3. session_manager 分配 `wire_tag`，通过 `pwos_rpc_retag()` 把 `call_id` 改成 wire_tag。
4. 响应回来后，RX consumer 调 `pwos_session_manager_deliver_data(DATA_RPC, src, call_id, payload)`。
5. pending 命中后唤醒等待方，RPC client 解码 RESPONSE 并校验 `call_id`。
6. 超时后 RPC client 会尽力发送 CANCEL。

关键代码：

| 责任 | 代码 |
|---|---|
| RPC call_id retag | `pwos-master-esp32p4/slave_rpc/rpc_client.c:8` |
| unary call | `rpc_client.c:78` |
| 超时 cancel | `rpc_client.c:31`, `rpc_client.c:148` |
| stream 请求 | `rpc_client.c:176` |
| chunk 聚合与顺序检查 | `pwos-master-esp32p4/host_sessions/session_manager.c:762` |

### Job 提交与查询

1. `pwos_job_manager_submit()` 先分配本地 `host_job_id` 和 job 槽位，状态置 `ASSIGNED`。
2. 发送 `PWOS_JOB_KIND_SUBMIT`，协议里的 `request_id` 先为 0。
3. session_manager retag 后用 `request_id == wire_tag` 匹配响应。
4. STM32 返回 `SUBMIT_ACK`，其中 `job_id` 是远端 `remote_job_id`。
5. 后续 `status/result/cancel` 都通过 `remote_job_id` 找远端任务。
6. 传输失败、节点重启或远端 NOT_FOUND 会把本地 job 标记为 `LOST`；retry 会创建新 host job，不复活旧 job。

关键代码：

| 责任 | 代码 |
|---|---|
| Job 槽位复用策略 | `pwos-master-esp32p4/host_jobs/job_manager.c:58` |
| Job 请求统一封装 | `job_manager.c:82` |
| submit 生命周期 | `job_manager.c:260` |
| status/result/cancel 共用路径 | `job_manager.c:336` |
| retry 创建新 job | `job_manager.c:484` |
| 节点丢失转 LOST | `job_manager.c:564` |

## session_manager 必须讲清楚

### 它解决的问题

主机同一时间可能有 mini9P 文件访问、RPC、Job 请求。它们都经过同一条 UART/link 数据面，如果只按 tag 匹配，很容易跨协议或跨节点误投递。`session_manager` 做三件事：

1. 维护每个 MCU 地址一个 session 和 mini9P client。
2. 维护全局 pending 表，匹配键是 `(data_type, src_addr, wire_tag)`。
3. 处理 deadline、节点重启、流式响应聚合和统计。

### typed pending

pending 的核心字段在 `session_manager.h:104` 开始：

| 字段 | 作用 |
|---|---|
| `data_type` | 区分 DATA_MINI9P、DATA_RPC、DATA_JOB |
| `src_addr` | 响应必须来自目标 MCU 地址 |
| `wire_tag` | 每次请求唯一递增，mini9P tag/RPC call_id/Job request_id 都会改成它 |
| `boot_id` | 记录请求发出时的节点 boot，响应回来时还要校验 |
| `streaming` | 普通响应和流式响应走不同 deliver 函数 |
| `response[]` | 固定 512 字节缓冲，避免动态内存 |

典型追问答案：

**为什么 pending 只有 8 个？**  
这是嵌入式主机侧的固定并发上限，避免动态内存和不可控延迟。真实并发还受到应用层串行化限制，尤其同一节点的 mini9P client 会被 client mutex 串行。

**不同节点能并发吗？**  
能。每个 session 有自己的 `client_lock(session_index)`，同一节点串行，不同节点的 client lock 不同。测试 `test_two_nodes_are_concurrent_and_tags_are_global()` 验证两个节点 attach 可并发，并且 wire_tag 全局不同。

**响应怎么避免错配？**  
普通响应在 `pwos_session_manager_deliver_data()` 中同时检查 `data_type`、`src_addr`、`wire_tag`。任何一个不匹配都会记为 unmatched，不会唤醒错误 pending。

**节点重启时未完成 pending 怎么办？**  
`pwos_session_manager_update_node()` 发现同地址 `boot_id` 变化后，把 session 置 `resetting`，调用 `cancel_pending_for_session_locked()` 给旧 pending 标记 `PWOS_SESSION_ERR_STALE_BOOT` 并 signal，随后 reset mini9P client。等待方会收到 stale_boot，而不是继续等超时。

## cluster_vfs 必须讲清楚

### route 表和 `/mcuN`

`cluster_vfs` 从 coordinator 节点表同步 route。它不是按地址命名，而是按 UID 复用 route：

- 新 UID 分配最小可用 `mcuN`。
- 同 UID 重启或换地址，仍保留原 `mcuN`。
- 地址被不同 UID 复用时，旧 route 下线，旧 fd 和 session 被清理。

核心在 `sync_one_node()`，代码位置 `pwos-master-esp32p4/host_api/cluster_vfs.c:300`。

### generation 的意义

`route.generation` 是 route 身份版本。只要 `addr/boot_id/state` 发生会影响数据面一致性的变化，就递增 generation。open 得到的 fd 会保存 `route_generation`，read/write/close 前后都检查：

- 打开前 route 必须在线。
- I/O 前 fd 的 `route_generation` 必须等于当前 route。
- 等待远端响应回来后再检查一次，防止链路等待期间节点重启。

所以旧 fd 不会在节点重启后继续读新节点。

### 路径解析

`resolve_path()` 做严格段匹配：

- `/mcu1/sys/health` 命中 `mcu1`，远端路径是 `/sys/health`。
- `/mcu10/...` 不会误命中 `mcu1`，因为要求后面是 `/` 或字符串结束。
- 若 `mcu1` 名字存在但离线，返回 `PWOS_SESSION_ERR_NO_ROUTE`。
- 若没有这个名字，返回 `ENOENT`。

典型追问答案：

**`/mcuN` 名字重启后会变吗？**  
同一个 UID 不会变。`cluster_vfs` 用 UID 找旧 route，而不是用短地址。短地址只是租约结果，可能复用。

**多个用户同时 cat 同一个节点文件会冲突吗？**  
不同 fd 之间不会共享 remote_fid；同一 fd 被 `busy` 标记保护。同一节点 mini9P client 通过 session_manager 的 client mutex 串行，避免 client 内部 tag/fid 状态并发错乱。

**为什么 VFS 不直接发 link frame？**  
VFS 只负责命名和 fd 映射，传输统一交给 session_manager。这样 mini9P/RPC/Job 都共享同一套 pending、deadline、boot_id 清理逻辑。

## mini9P 必须讲清楚

### mini9P 帧格式

线格式在 `m9p_encode_frame()` / `m9p_decode_frame()`：

```text
magic(2) = '9''P'
len(2)   = version/type/tag/payload 的长度
version(1)
type(1)
tag(2)
payload(N)
crc(2)   = CRC-16/CCITT-FALSE(version/type/tag/payload)
```

注意：`M9P_FRAME_OVERHEAD = 10`，即 magic2 + len2 + version/type/tag4 + crc2。

### 支持的操作

mini9P 支持：

- attach：建立会话，协商 msize/inflight/features/root qid。
- walk：从 fid 走到 path，绑定 newfid。
- open：按模式打开 fid。
- read/write：按 fid + offset 读写。
- stat：查询属性。
- clunk：释放 fid。

典型追问答案：

**qid 是什么？**  
qid 是文件对象标识，包含 type、version、object_id。type 标记目录/虚拟文件/设备/计算/只读；object_id 由后端给出，用来区分对象。

**为什么 retag 后要重算 CRC？**  
mini9P 的 tag 位于 CRC 覆盖范围内。`m9p_retag_frame()` 修改 tag 后必须重算帧尾 CRC，否则 `m9p_decode_frame()` 会失败。

## RPC 必须讲清楚

RPC 是短调用/流式调用/通知/取消的主机侧封装，底层仍走 session_manager。

| 模式 | 函数 | 特点 |
|---|---|---|
| unary | `pwos_rpc_client_call()` | REQUEST -> RESPONSE |
| stream | `pwos_rpc_client_stream()` | REQUEST(STREAM) -> STREAM_CHUNK* -> STREAM_END |
| notify | `pwos_rpc_client_notify()` | ONEWAY，不占 pending |
| cancel | `send_cancel()` | deadline 后尽力发送 CANCEL |

流式顺序保证不在 RPC client 内，而在 `session_manager_deliver_data_part()`：非 final chunk 的 `status_or_part_index` 必须等于当前 `stream_parts`，否则返回 EIO，防止丢帧、重复和乱序产生残缺结果。

## Job 必须讲清楚

### 两个 job id

| 字段 | 谁分配 | 用途 |
|---|---|---|
| `host_job_id` | 主机 | 用户命令 `status/result/cancel/retry` 使用 |
| `remote_job_id` | STM32 | 后续 Job protocol 请求带给远端 |

submit 先创建本地 job，即使发送超时也保留一条 LOST 记录，方便用户看见失败并 retry。

### 状态

Job 状态来自 `pwos-shared/job/pwos_job_protocol.h`：

`EMPTY, CREATED, QUEUED, ASSIGNED, RUNNING, DONE, FAILED, CANCELLED, LOST`

其中 `LOST` 是主机侧非常关键的状态：

- submit/status/result/cancel 传输失败时，主机不知道远端是否还活着，标 LOST。
- 远端返回 `NOT_FOUND`，说明 remote_job_id 丢失，标 LOST。
- 节点重启或离线，`pwos_job_manager_mark_node_lost()` 把旧 boot 上所有非终态 job 标 LOST。
- LOST 不允许被新 boot 的同号 remote job 复活，只能 retry 创建新 host job。

## 高频追问与参考答案

**Q：数据面请求完整路径是什么？**  
A：用户路径或命令先到 `cluster_vfs/rpc_client/job_manager`，这些模块编码 mini9P/RPC/Job 内层帧，然后统一进 `session_manager`。session_manager 分配 wire_tag、建立 pending，通过 runtime 的 send 回调发 DATA_MINI9P/DATA_RPC/DATA_JOB。RX consumer 收到响应后按 `(data_type, src_addr, wire_tag)` deliver，等待方被唤醒并解码结果。

**Q：如果节点重启时还有未完成 pending？**  
A：`update_node()` 发现 boot_id 变化后取消该 session 的 pending，状态是 `PWOS_SESSION_ERR_STALE_BOOT`。mini9P client attach 状态也清空，下一次访问会重新 attach。

**Q：为什么 `acquire_client/release_client` 必须成对？**  
A：`acquire_client()` 会拿该 session 的 `client_lock`，保护 mini9P client 的 tag/fid/tx_buffer/rx_buffer。忘记 release 会死锁同节点后续 mini9P 操作。

**Q：RPC 流式响应如何保证 chunk 顺序？**  
A：每个非 final chunk 的 `status` 字段承载从 0 开始的 chunk index。session_manager 维护 `stream_parts`，收到的 index 必须连续，否则 pending 结束为 EIO。

**Q：Job 提交失败会重试吗？**  
A：submit 本身不自动重试。失败后仍产生一个 LOST 的 host job 记录，用户或上层调用 `retry`，它会用旧 input 创建一个新的 host_job_id 和新的远端 job。

**Q：deadline 发生后会怎样？**  
A：session_manager 释放 pending，迟到响应会 unmatched。RPC client 额外发送 CANCEL；Job manager 把本地 job 标为 LOST；VFS 调用返回 deadline 错误。

**Q：同一个地址被另一个节点复用了怎么办？**  
A：`cluster_vfs` 同步时发现相同 addr 但 UID 不同，会把旧 route 下线、清理旧 fd、reset 旧 session，再给新 UID 建立/更新 route。

**Q：为什么不用动态内存？**  
A：主机数据面也延续嵌入式固定上限思路：session 32、pending 8、open fd 16、job 16。固定数组让最坏情况内存和延迟可预期，便于上板调试。

## 代码阅读顺序

1. `pwos-master-esp32p4/host_sessions/session_manager.h`  
   先看结构体字段，尤其 `pwos_session_pending_t`。
2. `pwos-master-esp32p4/host_sessions/session_manager.c`  
   按 `reserve_pending_locked -> session_transport -> deliver_data -> request_data_internal -> update_node` 读。
3. `pwos-master-esp32p4/host_api/cluster_vfs.h/.c`  
   看 `route/file/generation`，再读 `resolve_path -> ensure_attached -> open/read/write/close`。
4. `pwos-shared/mini9p/mini9p_protocol.h/.c`  
   掌握帧格式、attach/walk/open/read/write/stat/clunk。
5. `pwos-master-esp32p4/slave_rpc/rpc_client.c`  
   看 unary/stream/notify/cancel 如何复用 session_manager。
6. `pwos-master-esp32p4/host_jobs/job_manager.c`  
   看 submit/status/result/cancel/retry 和 LOST 语义。
7. 测试文件：  
   `pwos-master-esp32p4/host_sessions/tests/test_session_manager.c` 验证并发、错配、deadline、boot change、RPC/Job；  
   `pwos-master-esp32p4/host_api/tests/test_cluster_vfs.c` 验证路径映射、重启 reattach、离线 no_route。

## 你应能现场画出的图

```text
cat /mcu1/sys/health
        |
        v
cluster_vfs: /mcu1/sys/health -> addr=1, boot=10, remote=/sys/health
        |
        v
mini9P client: TATTACH/TWALK/TOPEN/TREAD/TCLUNK
        |
        v
session_manager: reserve pending, retag, wait
        |
        v
DATA_MINI9P link frame to MCU
        |
        v
RX consumer deliver by (DATA_MINI9P, src=1, wire_tag)
```

```text
rpc system.ping
        |
        v
rpc_client: encode REQUEST(call_id=0)
        |
        v
session_manager: call_id <- wire_tag, pending key=(DATA_RPC, addr, wire_tag)
        |
        v
DATA_RPC to MCU
        |
        v
deliver_data -> decode RESPONSE -> status/payload
```

```text
job submit mcu1 hash hello
        |
        v
job_manager: allocate host_job_id, state=ASSIGNED
        |
        v
encode SUBMIT(request_id=0), retag to wire_tag
        |
        v
SUBMIT_ACK returns remote_job_id, state=QUEUED/RUNNING
        |
        v
status/result/cancel use host_job_id -> remote_job_id
```

## 已添加源码注释的重点文件

我已在以下文件中补充了答辩导向注释，打开源码时优先读这些注释：

- `pwos-master-esp32p4/host_sessions/session_manager.h`
- `pwos-master-esp32p4/host_sessions/session_manager.c`
- `pwos-master-esp32p4/host_sessions/tests/test_session_manager.c`
- `pwos-master-esp32p4/host_api/cluster_vfs.h`
- `pwos-master-esp32p4/host_api/cluster_vfs.c`
- `pwos-master-esp32p4/host_api/tests/test_cluster_vfs.c`
- `pwos-shared/mini9p/mini9p_protocol.h`
- `pwos-shared/mini9p/mini9p_protocol.c`
- `pwos-shared/mini9p/mini9p_client.h`
- `pwos-shared/mini9p/mini9p_client.c`
- `pwos-shared/mini9p/mini9p_server.h`
- `pwos-shared/mini9p/mini9p_server.c`
- `pwos-shared/mini9p/test_mini9p_client_host.c`
- `pwos-shared/rpc/pwos_rpc_protocol.h`
- `pwos-shared/rpc/pwos_rpc_protocol.c`
- `pwos-master-esp32p4/slave_rpc/rpc_client.c`
- `pwos-master-esp32p4/host_jobs/job_manager.c`
- `pwos-master-esp32p4/host_jobs/job_command.h`
- `pwos-master-esp32p4/host_jobs/job_command.c`
- `pwos-shared/job/pwos_job_protocol.h`
- `pwos-shared/job/pwos_job_protocol.c`
