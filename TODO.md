# TODO

## Mini9P 多 UART 与共享 backend 串行化

- 当前 `mini9p_service` 默认安全前提是单 UART、单 `m9p_server`、单线程轮询访问 backend。
- 如果未来多个 UART/service 同时访问同一个 `node_vfs/lfs_vfs/dev_vfs`，需要引入 backend 串行化机制。
- 推荐方向：每条 UART 保持独立 `m9p_server/mesh_node_runtime/transport`，共享 backend 通过单 backend worker 队列串行处理。
- `mini9p_service_backend` 当前可注入 worker proxy ops，但它本身不提供队列、锁、超时或取消语义。
- 后续可把 `uart` 从 `mini9p_service_backend` 拆到 `mini9p_service_config`，让 backend 注入和 transport 配置职责分离。

## MCU1 双 UART 中继缺失

- 当前硬件拓扑：PC -- `mcu1` USART2 @ 1000000；`mcu1` USART1 @ 115200 -- `mcu2` USART1 @ 115200。
- 现象：`mcu1` 单独上板正常，`mcu2` 单独上板正常；但在链式拓扑下 reset `mcu2` 后，PC 收不到 `mcu2` 的 REGISTER。
- 当前代码根因：`pwos-slave` 初始化了 USART1/USART2，但 `mini9p_board_service` 只把 mesh service 绑定到 `huart2`；`huart1` 没有 mesh transport/runtime 轮询，因此不能接收或转发 `mcu2` 的帧。
- 共享 `mini9p_service` 目前也是单 `mesh_uart_transport` + 单 `mesh_node_runtime` 全局实例，不支持 mcu1 同时作为上游节点和下游中继。
- 后续实现方向：为 mcu1 增加双端口 mesh bridge/runtime，分别管理 USART2 上游和 USART1 下游；按端口轮询收帧，并在 `send_frame(next_hop, ...)` 中按下一跳选择输出 UART。
- 注意：当前 `mesh_processer` 会把 `dst=0xff` 的 bootstrap REGISTER 当作本机控制帧处理。中继模式下需要明确 mcu1 对下游 bootstrap REGISTER 的策略：转发到 PC、代理注册，或新增专门 bridge 逻辑。
