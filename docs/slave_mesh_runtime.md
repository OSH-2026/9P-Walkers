# 从机 Mesh2 Runtime 指南

## 数据流

```text
UART ReceiveToIdle DMA ISR
  -> link_rx queue
  -> link_rx_task / port_manager
  -> mesh_rx queue
  -> mesh_ctrl_task / node_control
  -> service_rx queue
  -> service_task / mini9P server / local_vfs
```

发送统一经过 `ctrl_tx` 或 `link_tx` 队列，再由唯一 `link_tx_task` 调用
`pwos_uart_dma_send()`。控制面队列优先，TX 完成后保留 2 ms 帧间隔，覆盖接收端
ReceiveToIdle DMA 的重新 arm 空窗。

## 模块所有权

- `uart_dma_port`：HAL UART/DMA、parser、RX rearm、TX completion 和硬件统计。
- `port_manager`：每个物理端口的 HELLO FSM、peer UID/boot/capability。
- `node_control`：本机地址、lease、上游、路由、REGISTER 中继和数据转发。
- `service_runtime`：唯一 mini9P server，会话/fid 和本机诊断内容。
- `local_vfs`：固定 `/sys` 命名空间、权限、qid 和完整 dirent 分页。
- `fault_control`：Debug 构建按端口执行故障注入。

## Bootstrap

1. 未分配节点从 `port_manager` 选择 coordinator 或声明 upstream-reachable 的 peer。
2. `node_control` 周期发送 `CTRL_NODE_REGISTER`。
3. relay 记录下游 UID/boot 与 ingress port，并向上游转发 REGISTER。
4. coordinator 返回 `CTRL_ADDR_ASSIGN`；relay 按 pending 记录转发给下游。
5. 节点固定 upstream，周期续租并上报直连 LINK_STATE。
6. coordinator 下发 ROUTE_UPDATE，`node_control` 写入 `(dst,next_hop,port)` 路由。

## 关键约束

- UART RX 只有 `link_rx_task` 一个消费者。
- ISR 只投递帧块或任务通知，不运行 port/mesh/mini9P 业务。
- 帧块入队成功即转移所有权；失败路径必须归还 `frame_pool`。
- mini9P 请求只在 `service_task` 串行处理，响应通过 `node_control` 查路由发送。
- `port_manager`、`node_control` 对外只提供临界区保护的快照 API。

## 诊断

```text
/sys/health     /sys/tasks      /sys/ports      /sys/links
/sys/neighbors  /sys/routes     /sys/sessions   /sys/queues
/sys/log        /sys/build      /sys/fault
```

Debug 构建可由主机 WebShell 执行：

```text
fault mcu2 drop port 0 20
fault mcu2 corrupt port 0 10
fault mcu1 down port 0
fault mcu1 recover port 0
fault mcu2 reboot-self
fault mcu2 clear
```

## 相关文件

- `pwos-slave/User/drivers/uart_dma_port.c`
- `pwos-slave/User/link/port_manager.c`
- `pwos-slave/User/mesh2/node_control.c`
- `pwos-slave/User/service/service_runtime.c`
- `pwos-slave/User/backend/local_vfs.c`
- `pwos-slave/User/rtos/pwos_tasks.c`
- `pwos-slave/User/diag/fault_control.c`
