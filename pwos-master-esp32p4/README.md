# PWOS ESP32-P4 Coordinator

`pwos-master-esp32p4` 当前默认固件运行 M4 coordinator 控制面，并接入 M5
`session_manager + cluster_vfs`。P4 会通过统一的 `/mcuN/...` 路径周期读取
各节点 `/sys/health`，验证 `DATA_MINI9P` 数据面闭环。不再启动旧的 Lua、
WebShell、LAN、legacy mesh host service 或旧 VFS bridge。

## 当前职责

- 通过 `UART1 TX=37 RX=38 baud=1000000` 接入 STM32 节点链路。
- 周期发送 coordinator `LINK_HELLO`，让从机 `port_manager` 选择 host upstream。
- 接收 `CTRL_NODE_REGISTER` 并返回 `CTRL_ADDR_ASSIGN`。
- 接收 `CTRL_LEASE_RENEW` 并返回 `CTRL_LEASE_ACK`。
- 接收 `CTRL_LINK_STATE`，在可推导时下发 `CTRL_ROUTE_UPDATE`。
- 同步 coordinator 节点表到 `cluster_vfs`，为每个 UID 稳定分配 `mcuN` 名称。
- 通过 `session_manager` 维护 per-node mini9P client、boot_id reset 和 deadline 错误。
- 周期执行 `cluster_vfs_read_path("/mcuN/sys/health")`，验证数据面转发闭环。
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
│   └── session_manager.h
├── host_api/
│   ├── cluster_vfs.c
│   ├── cluster_vfs.h
│   └── tests/
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
```

## 下一步

当前 M5 已有正式 `session_manager + cluster_vfs`，但仍由 coordinator 单任务串行驱动。
下一步进入 M6：把接收侧改成唯一 poll consumer + pending 表，支持并发上层请求而不串响应。
