# PWOS STM32F407 从机

`pwos-slave` 当前是 STM32F407 + FreeRTOS 从机固件。旧裸机
`mesh_node_runtime/mesh_node_service` 已删除；当前路径是：

```text
UART DMA/IDLE ISR
  -> pwos_queues
  -> link_rx_task / port_manager
  -> mesh_ctrl_task / node_control
  -> service_task / mini9P server / RPC service / Job service / local_vfs
  -> compute_task / compute_worker
```

## 当前目录

```text
pwos-slave/
├── Core/                 # CubeMX 生成代码，已启用 FreeRTOS
├── User/
│   ├── drivers/          # uart_dma_port
│   ├── link/             # port_manager
│   ├── mesh2/            # node_control
│   ├── service/          # mini9P server + DATA_RPC/DATA_JOB service
│   ├── compute/          # 独立低优先级 worker 与静态 kernel
│   ├── backend/          # local_vfs 诊断命名空间
│   ├── diag/             # Debug 故障注入
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
- `service_runtime` 接收本机 `DATA_MINI9P`，通过 `local_vfs` 暴露完整 `/sys` 诊断树。
- `service_runtime` 同时接收 `DATA_RPC`；内置
  `system.ping/stream/info/notify/delay/fail`。流式响应按 32 字节分块并以 10 ms
  间隔发送，延期调用由 service task 轮询推进，不阻塞 mesh 控制面。
- `service_runtime` 接收 `DATA_JOB` 控制帧，独立低优先级 compute task 执行
  hash/vector_add/matmul/mandelbrot；job 支持进度、结果与取消。
- `/compute/caps`、`/compute/load`、`/compute/jobs` 暴露计算能力、负载和静态 job
  快照。
- `/sys/tasks`、`ports`、`links`、`neighbors`、`routes`、`sessions`、`queues`、
  `log`、`build` 均读取真实运行时快照。
- Debug 构建可写 `/sys/fault`，按端口注入 drop/delay/corrupt/down/recover。

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

执行 `rpc mcuN system.ping hello` 返回 `hello`，并且 `/sys/sessions` 中 RPC
request/response 计数增长时，说明 `DATA_RPC` 闭环。

执行 `job caps mcuN`、`job submit mcuN matmul`，随后 `job result <id>` 返回
`[19,22;43,50]`，并且 `/compute/jobs` 状态为 done 时，说明 `DATA_JOB` 闭环。
