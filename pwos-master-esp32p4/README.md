# PWOS ESP32-P4 Coordinator

`pwos-master-esp32p4` 当前默认固件运行 M4-M8 coordinator、session、并发 VFS、
从机 RPC、有线 LAN 和异步 WebShell。
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
- coordinator RX 任务是唯一 UART 接收消费者，探测 worker 只等待 semaphore。
- `cluster_vfs` 用 mutex 和 generation 保护 route/fd，链路等待期间不持全局锁。
- TX 帧之间保留 2 ms guard，覆盖 STM32 ReceiveToIdle 的 DMA rearm 空窗。
- 周期执行 `cluster_vfs_read_path("/mcuN/sys/health")`，验证数据面转发闭环。
- 使用 ESP32-P4 Function EV Board 的 IP101 PHY（地址 1、reset GPIO 51）接入 LAN。
- HTTP 提供 `/`、`/health`、`/ws`，mDNS 主机名为 `pwos.local`。
- WebSocket 回调只入队，单独 worker 执行命令；结果用 fd generation 隔离客户端。
- 本地 `/host/sys/{health,links,topology,routes,sessions,web,log}` 提供状态快照。
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
├── host_rpc/             # DATA_RPC client
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
```

## WebShell

DHCP 成功后打开 `http://pwos.local/` 或串口日志中的 IPv4 地址。常用命令：

```text
ls /
cat /mcu1/sys/health
cat /host/sys/sessions
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
```

## 下一步

M8 unary 已上板通过。按 `docs/logs/refactor/M8-slave-rpc.md` 完成 streaming
多 chunk、流式 deadline 和恢复测试；通过后进入 M9 Job System。
