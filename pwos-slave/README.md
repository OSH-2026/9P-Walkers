# PWOS STM32F407 从机

`pwos-slave` 当前是 STM32F407 + FreeRTOS 从机固件。旧裸机
`mesh_node_runtime/mesh_node_service` 已删除；当前路径是：

```text
UART DMA/IDLE ISR
  -> pwos_queues
  -> link_rx_task / port_manager
  -> mesh_ctrl_task / node_control
  -> service_task / mini9P server / local_vfs
```

## 当前目录

```text
pwos-slave/
├── Core/                 # CubeMX 生成代码，已启用 FreeRTOS
├── User/
│   ├── drivers/          # uart_dma_port
│   ├── link/             # port_manager
│   ├── mesh2/            # node_control
│   ├── service/          # DATA_MINI9P -> mini9P server
│   ├── backend/          # local_vfs / 后续 node_vfs
│   ├── os/               # frame_pool
│   └── rtos/             # 任务和队列
├── CMakeLists.txt
├── CMakePresets.json
└── build.sh              # build / flash
```

## 构建

```bash
cd pwos-slave
cmake --preset F407Debug
cmake --build --preset F407Debug
```

辅助脚本：

```bash
pwos-slave/build.sh build
pwos-slave/build.sh flash
```

## 当前功能

- USART1/USART2 等 CubeMX 已启用端口通过 `uart_dma_port` 接入链路层。
- `port_manager` 处理 link hello / hello_ack，识别 host、node、peer。
- `node_control` 完成 REGISTER、ASSIGN、RENEW、LINK_STATE、ROUTE_UPDATE 和数据面转发。
- `service_runtime` 接收本机 `DATA_MINI9P`，当前挂载 `local_vfs`，暴露 `/`、`/sys`、`/sys/health`。

## 上板检查

GDB 常用观察点：

```gdb
print 'port_manager.c'::g_ports[0]
print 'port_manager.c'::g_ports[1]
print 'node_control.c'::g_stats
print 'service_runtime.c'::g_service.stats
print ((pwos_uart_dma_port_t *)'uart_dma_port.c'::g_ports)[0].stats
```

P4 侧看到 `mini9p mcuN addr=N /sys/health=ok` 时，说明该节点控制面和
`DATA_MINI9P` 数据面都已经闭环。
