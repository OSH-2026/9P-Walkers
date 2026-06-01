# TODO

## STM32 Mesh UART DMA + 事件队列化

目标一句话：DMA 只搬字节，中断只登记事件，队列只缓存待处理工作，`mesh_node_service_poll_once()` / 主循环继续负责真正的 mesh 和 Mini9P 业务。

### 当前问题

- STM32 HAL transport 现在仍用 `HAL_UART_Receive()` 阻塞式读完整 raw mesh frame。
- `rx_pending + poll_once` 已经避免在空 UART 上固定阻塞 1ms，但一旦某个端口开始读半帧，仍可能卡住其它端口。
- 多 UART 串联场景里，中继节点 A 同时面对上游 PC 和下游 B；如果 RX 处理被某一端口拖住，就容易延迟 probe response、LINK_STATE 或 Mini9P 响应。
- 现有架构已经有清晰业务边界：`mesh_uart_transport_*` 负责 transport，`mesh_node_service_receive_frame()` 负责选择 ingress port，`mesh_node_runtime_poll_once()` 负责业务分流；DMA 化应保留这个边界。

### 目标架构

```text
USARTx RX
    ↓
DMA 写入 port-local rx_dma_buf
    ↓
IDLE / half / complete IRQ
    ↓
只记录 rx_event(port, write_pos)
    ↓
轻量事件队列或 pending bitset
    ↓
mesh_node_service_poll_once()
    ↓
mesh_uart_transport_receive_frame()
    ↓
从 port-local ring/frame queue 取完整 raw mesh frame
    ↓
mesh_node_runtime_poll_once() 处理 REGISTER/ASSIGN/ROUTE_UPDATE/Mini9P
```

### 设计原则

- ISR / DMA callback 不解析 mesh，不调用 `mesh_decode_frame()`，不调用 `mesh_node_runtime_*()`，不访问 `node_vfs/lfs_vfs/dev_vfs`。
- ISR 只做：读取 DMA 写指针、记录端口事件、清错误/IDLE 标志、必要时重启 DMA。
- transport 层把字节流整理成完整 raw mesh frame；业务层仍只看到 `send_frame/receive_frame` 回调。
- `mesh_uart_transport_rx_pending()` 保留，DMA 化后语义改为“该端口已有完整帧或待推进的 RX 数据”。
- TX 第一阶段保持阻塞 `HAL_UART_Transmit()`，先解决 RX 并发和丢帧；确认瓶颈后再评估 TX DMA。
- 所有缓冲区固定大小、静态分配，不引入 malloc。

### 分阶段方案

1. **板级 DMA/NVIC 准备**
   - 在 F407/F411 CubeMX 或等价手工代码中启用 USART1/USART2 RX DMA 和 IDLE interrupt。
   - 保持 USART2 为 PC/host 主口，USART1 为下游级联口。
   - 不把 VOFA 文本输出接到 mesh 二进制口。

2. **transport RX 缓冲**
   - 在 `struct mesh_uart_transport` 的 STM32 分支增加 per-port RX 状态：DMA buffer、read offset、write offset、完整帧缓存或 frame queue。
   - 支持从字节流中寻找 raw mesh magic `'M''H'`、读取长度、校验 frame size，再产出完整帧。
   - 坏帧只丢弃到下一个可能 magic，避免直接 flush 整个端口导致丢后续好帧。

3. **ISR 到主循环的事件入口**
   - 提供 STM32 专用入口，例如 `mesh_uart_transport_on_rx_event(UART_HandleTypeDef *uart, size_t dma_pos)` 或等价 dispatcher。
   - 如果不做通用队列，第一版可用 per-port pending bitset；队列满/溢出必须计数并进 `/sys/log`。
   - callback 只标记端口，不做 frame decode。

4. **保持现有 service/runtime 模型**
   - `mesh_node_service_receive_frame()` 继续 round-robin 扫描端口。
   - `mesh_uart_transport_receive_frame()` 改为从内存队列取完整帧；无完整帧返回 `-MESH_ERR_BUSY`。
   - `mesh_node_runtime_poll_once()` 和 Mini9P server/backend 不感知 DMA。

5. **可选：统一任务队列**
   - 当事件类型变多（日志 flush、timer tick、按键、ADC、SPI）时，再引入全局 `pwos_task_queue`。
   - 初期只把 mesh UART RX 做成 transport-local pending/event，不急于引入全局调度器。
   - 如果未来迁移 RTOS，`pwos_task_queue` 可替换为 queue/semaphore/task notify，mesh 业务接口不推倒重来。

### 需要改动的区域

- F407/F411 USART DMA/NVIC 初始化。
- `pwos-shared/mesh/transport/mesh_uart_transport.[ch]` 的 STM32 HAL 分支。
- `pwos-slave/User/mesh/mesh_node_service.c` 中 RX pending/receive 的错误处理和统计。
- `/sys/log` 增加 RX overflow、bad frame、queue full 等诊断事件。
- PC/host test 或新增 host-side transport parser test，覆盖 partial frame、back-to-back frame、bad magic recovery。

### 验收场景

- 单板 `PC <-> F407`：REGISTER、ASSIGN、NEIGHBOR_PROBE、LINK_STATE、ROUTE_UPDATE、attach、读取 `/sys/health` 成功。
- 单板 `PC <-> F411`：同上。
- 串联 `PC <-> A <-> B`：B 能注册为 `mcu2`，ASSIGN 能回转，A/B 都能上报 LINK_STATE，主机能读 `/mcu2/sys/health` 和 `/mcu2/sys/log`。
- A 的 USART1/USART2 同时收到短控制帧时，不丢 probe response，不出现长时间 `MESH_ERR_BUSY`。
- 连续 back-to-back Mini9P Tread/Rread 不截断、不串帧。
- 人为注入坏 magic/半帧后，transport 能恢复到下一帧，并在 `/sys/log` 记录错误计数。

### 暂不做

- 第一阶段不做 TX DMA。
- 第一阶段不引入 RTOS。
- 第一阶段不把 Mini9P/server/backend 改成多线程并发。


## 备选：邻居传播路由模型

- 当前路由策略先保持“控制器下发指定路由”：主机/控制器计算或指定 `dst -> next_hop -> metric`，节点只应用收到的路由更新。
- 后续可评估邻居传播模型：每个节点周期或触发式向直连邻居广播自己的可达表，接收方用“邻居宣告 metric + 1”与本地路由表比较，选择更优路由。
- 在该模型里，接收方的 `next_hop` 应该是宣告该路由的邻居地址，也就是入站控制帧的 `src`；不应直接信任 payload 里的 `next_hop`，否则会把邻居内部路径误当成本机下一跳。
- `addr -> port` 映射可以由入站端口学习，但应优先从直连邻居控制帧学习，不能简单用普通转发数据帧的原始 `src` 推断直连关系。
- 需要处理 count-to-infinity/环路收敛问题：metric 会增大但不代表立刻无环，需要设置最大 metric、路由老化、版本/序列号、split horizon 或 poison reverse。
- 如果以后实现，建议新增“route advertise”语义，避免和现有“控制器指定 route update”混用。

![alt text](image.png)

##  **拓扑有向**
- 目前`收到 `A → B` 不自动补 `B → A`；反向路径必须由 B自行上报 LINK_STATE
- 考虑将来：主机直接把这样的链路视做双向连通
