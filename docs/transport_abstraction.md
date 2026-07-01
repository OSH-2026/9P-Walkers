# 链路与 UART 运行时

旧的跨平台 `mesh_uart_transport` 已删除。当前共享层只提供纯协议
`pwos-shared/link`，硬件收发分别由 ESP-IDF coordinator 和 STM32 HAL 驱动实现。

## 1. 共享 Link 层

```text
pwos_link_crc       CRC-16/CCITT-FALSE
pwos_link_frame     link frame v2 编解码
pwos_link_parser    增量字节流 parser
```

这些模块不依赖 HAL、ESP-IDF、FreeRTOS 或具体 UART。

## 2. STM32 RX

```text
UART ReceiveToIdle DMA callback
  -> uart_dma_port 读取本次 DMA 数据
  -> pwos_link_parser_feed
  -> 完整 frame 复制到 frame_pool block
  -> link_rx queue
  -> link_rx_task
```

设计约束：

- ISR/回调不运行 port manager、路由、mini9P、RPC 或 Job。
- 每个完整帧使用固定块池，入队失败立即归还。
- RX rearm 在任务上下文完成；统计记录 event type、event size、UART error 和 restart。
- 一个 UART 的错误和恢复只影响该端口。

STM32 TX 统一进入 `ctrl_tx` 或 `link_tx` 队列，由唯一 `link_tx_task` 串行调用
DMA TX。控制面优先，帧间保留 guard time，避免接收端 rearm 空窗叠加。

## 3. ESP32 RX/TX

`pwos-master-esp32p4/coordinator_runtime` 是 P4/S3 共享的 UART coordinator 实现：

- 初始化 ESP-IDF UART driver。
- 单一 RX task 消费 UART 字节并驱动 link parser。
- coordinator 在同一任务中处理控制帧和分发 typed data 响应。
- 请求发起方只等待 session manager 的 semaphore，不直接读取 UART。
- TX 使用互斥和帧间 guard，防止多个上层并发写交错。

S3 通过编译定义覆盖 UART 端口和引脚；P4 使用板级默认配置。

## 4. 端口和路由

STM32 `port_manager` 维护物理端口 FSM：

```text
PROBING -> PEER / HOST_CANDIDATE -> SUSPECT -> PROBING
```

HELLO payload 携带 UID、boot ID、port ID、capability 和 peer role。`node_control`
维护 `(dst, next_hop, port, metric, version)` 路由；普通数据帧不能通过“观察 src”
隐式改写直连关系，邻接事实来自 HELLO 和控制面。

## 5. 诊断

```text
/sys/ports    port manager + UART DMA 统计
/sys/links    直连 peer
/sys/routes   当前转发表
/sys/queues   frame pool 和队列高水位
/sys/tasks    heartbeat 和 stack watermark
/sys/fault    Debug 故障注入
```

常见判断：

- `hello_rx` 增长但 `hello_ack_rx` 不增长：单向链路或对端 TX/本端 RX 路径异常。
- `rx_bytes` 增长而 `rx_frames` 不增长：帧不完整、版本/长度/CRC 或 parser 同步问题。
- `rearm_failures` 增长：HAL/DMA 状态未正确恢复。
- `rx_drop` 增长：块池或队列容量不足，需检查下游任务是否被阻塞。
