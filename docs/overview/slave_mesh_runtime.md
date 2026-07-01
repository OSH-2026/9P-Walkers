# STM32 节点运行时

F407 和 F429 使用同一套通信、控制、服务和计算模型；F429 额外提供显示 backend。

## 1. 任务流

```text
UART DMA callback
  -> link_rx queue
  -> link_rx_task
       -> port_manager
       -> mesh_rx queue
  -> mesh_ctrl_task
       -> node_control
       -> service_rx queue
  -> service_task
       -> mini9P / RPC / Job
  -> compute_task
       -> compute_worker

ctrl_tx/link_tx queue -> link_tx_task -> UART DMA TX
```

控制 TX 队列优先。每个任务有 heartbeat 和 stack watermark，可从 `/sys/tasks` 读取。

## 2. 模块所有权

- `uart_dma_port`：HAL UART/DMA、parser、rearm、TX complete、硬件统计。
- `frame_pool`：固定帧块生命周期。
- `port_manager`：HELLO FSM、peer UID/boot/capability 和端口健康。
- `node_control`：地址、lease、authority、route、时间映射、relay bootstrap 和数据转发。
- `service_runtime`：本机 typed data 分发和 mini9P server。
- `rpc_service`：MCU RPC 方法和延期/流式调用状态。
- `job_service`：远端 job 协议和静态 job 槽。
- `compute_worker`：低优先级 kernel 执行。
- `local_vfs`：路径、权限、qid 和运行时文本回调。
- `fault_control`：Debug 故障注入。

## 3. Bootstrap 和路由

1. 端口交换 HELLO/ACK。
2. 节点选择 host 或可达 upstream 的 peer。
3. 未分配节点周期发送 REGISTER。
4. relay 按 UID/boot/ingress 记录并转发下游 REGISTER。
5. ASSIGN 原路返回，节点保存地址和 lease epoch。
6. 节点上报直连 LINK_STATE。
7. coordinator 下发带 version 的 ROUTE_UPDATE。

节点只接受当前 authority/upstream 的地址、lease 和路由控制。旧 route version 不得覆盖
新版本。

## 4. 本地命名空间

```text
/sys
  health time tasks ports links neighbors routes sessions queues
  log build fault info uart

/compute
  caps load jobs

/display                 # 仅 F429
  status tile
```

`/sys/fault` 和 F429 `/display/tile` 是当前主要可写节点，其余诊断路径只读。

## 5. 计算

compute task 使用静态槽并逐步推进任务。当前 kernel：

- FNV-1a hash
- vector add
- 2x2 matmul
- Mandelbrot
- smallpt raytrace tile

service task 只做协议和状态转换，不等待 kernel 完成。因此计算期间 HELLO、lease、
route、health 和 RPC 仍应正常推进。

## 6. 调试

```text
cat /mcu1/sys/health
cat /mcu1/sys/time
cat /mcu1/sys/ports
cat /mcu1/sys/routes
cat /mcu1/sys/queues
cat /mcu1/compute/jobs

fault mcu1 status
fault mcu1 drop port 0 20
fault mcu1 corrupt port 0 10
fault mcu1 down port 0
fault mcu1 recover port 0
fault mcu1 clear
```

GDB：

```gdb
print 'port_manager.c'::g_ports[0]
print 'node_control.c'::g_stats
print 'service_runtime.c'::g_service.stats
print ((pwos_uart_dma_port_t *)'uart_dma_port.c'::g_ports)[0].stats
```

## 7. 不变量

- UART RX 只有 link task 消费。
- 帧块入队成功后所有权转移；所有失败路径必须归还块。
- 任一端口错误不能停止其他端口。
- service/compute 不直接操作 UART。
- 无 route 立即失败，不等待成 deadline。
- boot ID 改变后不接受旧启动实例的数据响应。
- wall-clock 是本地单调 tick 加同步偏移，不修改 HAL/FreeRTOS tick。
- deadline、lease 和重试不得使用 wall-clock。
