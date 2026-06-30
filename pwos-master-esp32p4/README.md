# PWOS ESP32-P4 Coordinator

`pwos-master-esp32p4` 当前默认固件运行 M4-M10 coordinator、session、并发 VFS、
从机 RPC、Job System、主机间 RPC、有线 LAN 和异步 WebShell。
P4 会通过统一的 `/mcuN/...` 路径周期读取
各节点 `/sys/health`，验证 `DATA_MINI9P` 数据面闭环。旧 Lua、legacy mesh host
service 和旧 VFS bridge 已删除；当前 WebShell 只接入新的 `cluster_vfs`。

## 当前职责

- 通过 `UART1 TX=37 RX=38 baud=1000000` 接入 STM32 节点链路。
- 周期发送 coordinator `LINK_HELLO`，让从机 `port_manager` 选择 host upstream。
- 接收 `CTRL_NODE_REGISTER` 并返回 `CTRL_ADDR_ASSIGN`。
- 接收 `CTRL_LEASE_RENEW` 并返回 `CTRL_LEASE_ACK`。
- 接收 `CTRL_LINK_STATE`，在可推导时下发 `CTRL_ROUTE_UPDATE`。
- 同步 coordinator 节点表到 `cluster_vfs`，为每个 UID 稳定分配 `mcuN` 名称。
- 通过 `session_manager` 维护 per-node mini9P client、boot_id reset 和 deadline 错误。
- 使用全局 wire tag 和 `(src_addr, tag)` pending 表匹配并发响应。
- mini9P 与 RPC 共用 pending 表，并按 `data_type` 隔离；RPC 支持 unary、流式聚合、
  单向通知、deadline 和超时后 CANCEL。
- `DATA_JOB` 与 mini9P/RPC 共用 typed pending；`job_manager` 跟踪进度、结果、取消、
  LOST 和 retry，保留 16 个有界静态 job 槽。
- coordinator RX 任务是唯一 UART 接收消费者，探测 worker 只等待 semaphore。
- `cluster_vfs` 用 mutex 和 generation 保护 route/fd，链路等待期间不持全局锁。
- TX 帧之间保留 2 ms guard，覆盖 STM32 ReceiveToIdle 的 DMA rearm 空窗。
- 周期执行 `cluster_vfs_read_path("/mcuN/sys/health")`，验证数据面转发闭环。
- 使用 ESP32-P4 Function EV Board 的 IP101 PHY（地址 1、reset GPIO 51）接入 LAN。
- HTTP 提供 `/`、`/health`、`/ws`，mDNS 主机名按 Ethernet MAC 生成为
  `pwos-xxxx.local`，避免多主机重名。
- `_pwos._tcp` 发布 TCP/9909；主机使用有界 CBOR RPC 执行 advertise、拓扑同步和
  跨主机节点 read/write。
- 主机按持久化 `epoch`、`priority`、`host_uid` 选出 leader；leader 统一分配全局
  `mcuN` 名称，follower 保留本机 `owner_target` 映射。
- coordinator 周期发送 `CTRL_HOST_ADVERTISE`；STM32 见到直接可达的 leader 后，只接受
  该入口端口发来的 lease、地址和路由控制帧。
- WebSocket 回调只入队，单独 worker 执行命令；结果用 fd generation 隔离客户端。
- 本地 `/host/sys/{health,links,topology,routes,sessions,jobs,hosts,web,log}` 提供状态快照。
- 通过日志输出节点数、收发帧、register、lease、route、mini9P、parse error 等统计。

## 目录结构

```text
pwos-master-esp32p4/
├── CMakeLists.txt
├── coordinator_runtime/
│   ├── pwos_coordinator_runtime.c
│   └── pwos_coordinator_runtime.h
├── host_coordinator/
│   ├── host_coordinator.c
│   ├── host_coordinator.h
│   └── tests/
├── host_sessions/
│   ├── session_manager.c
│   ├── session_manager.h
│   └── tests/
├── slave_rpc/            # MCU DATA_RPC client
├── host_rpc/             # TCP/mDNS/CBOR 主机间平面
├── host_jobs/            # DATA_JOB manager 与 WebShell job 命令
├── host_api/
│   ├── cluster_vfs.c
│   ├── host_observability.c
│   └── tests/
├── host_shell/          # 命令服务及 PC 单测
├── host_net/            # IP101 LAN runtime
├── web/                 # HTTP/WebSocket 与 index.html
├── main/
│   ├── CMakeLists.txt
│   ├── hello_world_main.c
│   └── idf_component.yml
├── sdkconfig
└── sdkconfig.defaults
```

## 构建

```bash
cd pwos-master-esp32p4
source /home/hb/.espressif/v6.0/esp-idf/export.sh
idf.py build
```

烧录和监视：

```bash
idf.py -p <PORT> flash monitor
```

## PC 单测

```bash
cmake -S pwos-master-esp32p4/host_coordinator/tests -B /tmp/pwos-host-coordinator-tests
cmake --build /tmp/pwos-host-coordinator-tests
/tmp/pwos-host-coordinator-tests/pwos_host_coordinator_test

cmake -S pwos-master-esp32p4/host_api/tests -B /tmp/pwos-host-api-tests
cmake --build /tmp/pwos-host-api-tests
/tmp/pwos-host-api-tests/pwos_host_api_test

cmake -S pwos-master-esp32p4/host_sessions/tests -B /tmp/pwos-host-session-tests
cmake --build /tmp/pwos-host-session-tests
/tmp/pwos-host-session-tests/pwos_host_session_test

cmake -S pwos-master-esp32p4/host_shell/tests -B /tmp/pwos-command-service-tests
cmake --build /tmp/pwos-command-service-tests
ctest --test-dir /tmp/pwos-command-service-tests --output-on-failure

cmake -S pwos-shared/host_rpc/tests -B /tmp/pwos-host-rpc-protocol-tests
cmake --build /tmp/pwos-host-rpc-protocol-tests
ctest --test-dir /tmp/pwos-host-rpc-protocol-tests --output-on-failure

cmake -S pwos-master-esp32p4/host_rpc/tests -B /tmp/pwos-host-rpc-endpoint-tests
cmake --build /tmp/pwos-host-rpc-endpoint-tests
ctest --test-dir /tmp/pwos-host-rpc-endpoint-tests --output-on-failure
```

## WebShell

DHCP 成功后打开串口日志给出的 `http://pwos-xxxx.local/` 或 IPv4 地址。常用命令：

```text
ls /
cat /mcu1/sys/health
cat /host/sys/sessions
hosts
net status
fault mcu2 status
fault mcu2 drop port 0 20
fault mcu2 clear
rpc mcu1 system.info
rpc mcu2 system.ping hello
rpc mcu2 system.delay 50 --deadline=500
rpc mcu2 system.delay 500 --deadline=50
notify mcu1 system.notify event
stream mcu2 system.stream 0123456789012345678901234567890123456789
job caps mcu1
job submit mcu1 hash hello
job submit mcu1 vector_add 8
job submit mcu2 matmul
job submit mcu2 mandelbrot 16 16 80
job list
job status <id>
job result <id>
job cancel <id>
job retry <lost-id>
```

## 下一步

按 `docs/logs/refactor/M10-host-rpc-multihost.md` 使用两块 P4、两个独立 MCU 子树完成
发现、选主、全局命名、跨主机 read/write 和断网切主验收。

## Lua 并行光线追踪

P4 启动后会创建 `lua_render` 任务，自动执行
`render/whitted_scheduler.lua`。脚本枚举当前可达 MCU，把 120x160 的
Whitted 经典场景拆成最大 8x7 的 tile，并让每个在线节点同时计算一个
`raytrace_tile` job。完成的 RGB565 tile 会写入带显示屏节点的
`/display/tile`，F429 再以 2 倍最近邻缩放持续更新 240x320 LCD。

运行时可用下面两个路径确认计算和显示进度：

```text
cat /mcu3/display/status
cat /mcu3/compute/jobs
```

P4 日志中每完成一帧会输出：

```text
pwos_lua_render: frame=1 complete tiles=345 workers=3 display=mcu3
```
