# 9P-Walkers

当前分支正在按 `docs/refactor_plan.md` 重构通信栈。旧裸机 mesh runtime、ESP32
旧 host runtime、Lua/WebShell、PC master emulator 和 `pwos-shared/mesh/` 已清理；
当前主线是：

```text
ESP32-P4 coordinator
  -> pwos-shared/link
  -> pwos-shared/mesh2
  -> DATA_MINI9P / DATA_RPC
  -> STM32F407 FreeRTOS node
```

## 当前能力

- STM32F407 多 UART 通过 DMA/IDLE 接入 `pwos-shared/link`。
- 从机 `port_manager` 自动发现 host/node peer。
- 从机 `node_control` 完成地址分配、lease、link-state、route-update 和数据面转发。
- P4 `host_coordinator` 维护节点地址/UID/boot_id。
- P4 `session_manager` 使用全局 tag 和 `(src_addr, tag)` pending 表分发响应。
- P4/STM32 已接入 `DATA_RPC` unary、streaming、fire-and-forget、cancel 和
  deadline；pending 按 `(data_type, src_addr, tag)` 隔离。
- P4 `cluster_vfs` 通过 generation + mutex 保护 `/mcuN/...` 路由和 fd 表。
- P4 仅 coordinator RX 任务读取 UART，不同节点的 mini9P 请求可并发等待。
- P4 通过 IP101 有线 LAN 提供 `http://pwos.local/` WebShell，WebSocket I/O
  与命令执行任务分离，输出按客户端隔离。
- 主机提供 `/host/sys/*`，从机提供完整 `/sys/*` 运行时诊断树。
- Debug 从机支持按端口 drop/delay/corrupt/down/recover 故障注入。

## 目录

```text
pwos-master-esp32p4/
  coordinator_runtime/   # P4 UART coordinator task
  host_coordinator/      # 地址/lease/route 控制面
  host_sessions/         # per-node mini9P session manager
  host_rpc/              # STM32 DATA_RPC client
  host_api/              # /host/sys/* + /mcuN/... 命名空间
  host_shell/            # 命令解析与路径适配
  host_net/              # ESP32-P4 IP101 LAN
  web/                   # HTTP/WebSocket 与嵌入页面
  main/                  # ESP-IDF component

pwos-slave/
  User/drivers/          # uart_dma_port
  User/link/             # port_manager
  User/mesh2/            # node_control
  User/service/          # mini9P server + DATA_RPC service registry
  User/backend/          # local_vfs 诊断命名空间
  User/diag/             # Debug 故障注入
  User/rtos/             # FreeRTOS task/queue

pwos-shared/
  link/                  # 链路帧、CRC、parser
  mesh2/                 # 控制面 payload 编解码
  mini9p/                # mini9P client/server/protocol
  rpc/                   # MCU DATA_RPC wire codec
  csc/                   # lttit 通信栈实验代码，暂未接入默认固件
```

## 构建

STM32F407：

```bash
cd pwos-slave
cmake --preset F407Debug
cmake --build --preset F407Debug
```

ESP32-P4：

```bash
cd pwos-master-esp32p4
source /home/hb/.espressif/v6.0/esp-idf/export.sh
idf.py build
```

## PC 测试

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

cmake -S pwos-shared/mesh2/tests -B /tmp/pwos-mesh2-tests
cmake --build /tmp/pwos-mesh2-tests
/tmp/pwos-mesh2-tests/pwos_mesh2_control_test

cmake -S pwos-shared/rpc/tests -B /tmp/pwos-rpc-tests
cmake --build /tmp/pwos-rpc-tests
ctest --test-dir /tmp/pwos-rpc-tests --output-on-failure

cmake -S pwos-slave/User/service/tests -B /tmp/pwos-rpc-service-tests
cmake --build /tmp/pwos-rpc-service-tests
ctest --test-dir /tmp/pwos-rpc-service-tests --output-on-failure
```

## 上板期望

正确连接示例：

```text
ESP32-P4 -> MCU1 USART2
MCU1 USART1 -> MCU2 USART1
```

P4 日志应出现：

```text
assign addr=1 ...
assign addr=2 ...
route owner=1 dst=2 ...
route owner=2 dst=1 ...
mini9p mcu1 addr=1 /sys/health=ok
mini9p mcu2 addr=2 /sys/health=ok
```

M8 unary 已完成上板验收，streaming 代码侧已完成。按
`docs/logs/refactor/M8-slave-rpc.md` 验证多 chunk 聚合、流式 deadline 和
mini9P/RPC 并行稳定性；通过后进入 M9 Job System。
