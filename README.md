# 9P-Walkers

当前分支正在按 `docs/refactor_plan.md` 重构通信栈。旧裸机 mesh runtime、ESP32
旧 host runtime、Lua/WebShell、PC master emulator 和 `pwos-shared/mesh/` 已清理；
当前主线是：

```text
ESP32-P4 coordinator
  -> pwos-shared/link
  -> pwos-shared/mesh2
  -> DATA_MINI9P
  -> STM32F407 FreeRTOS node
```

## 当前能力

- STM32F407 多 UART 通过 DMA/IDLE 接入 `pwos-shared/link`。
- 从机 `port_manager` 自动发现 host/node peer。
- 从机 `node_control` 完成地址分配、lease、link-state、route-update 和数据面转发。
- P4 `host_coordinator` 维护节点地址/UID/boot_id。
- P4 `session_manager + cluster_vfs` 通过 `/mcuN/...` 访问远端 mini9P。
- 当前上板 smoke：P4 周期读取 `/mcuN/sys/health`，看到 `ok` 表示控制面和数据面闭环。

## 目录

```text
pwos-master-esp32p4/
  coordinator_runtime/   # P4 UART coordinator task
  host_coordinator/      # 地址/lease/route 控制面
  host_sessions/         # per-node mini9P session manager
  host_api/              # cluster_vfs: /mcuN/... 命名空间
  main/                  # ESP-IDF component

pwos-slave/
  User/drivers/          # uart_dma_port
  User/link/             # port_manager
  User/mesh2/            # node_control
  User/service/          # DATA_MINI9P -> mini9P server
  User/backend/          # local_vfs / 后续 node_vfs
  User/rtos/             # FreeRTOS task/queue

pwos-shared/
  link/                  # 链路帧、CRC、parser
  mesh2/                 # 控制面 payload 编解码
  mini9p/                # mini9P client/server/protocol
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

cmake -S pwos-shared/mesh2/tests -B /tmp/pwos-mesh2-tests
cmake --build /tmp/pwos-mesh2-tests
/tmp/pwos-mesh2-tests/pwos_mesh2_control_test
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

下一步是 M6：把 P4 接收侧收敛为唯一 poll consumer，并用 pending 表支持并发
mini9P 请求。
