# mesh_uart_transport：三平台 UART 传输抽象

## 1. 定位

`mesh_uart_transport` 是 `pwos-shared/mesh/transport/` 中的平台无关 UART 传输层，向上层提供统一的“发送一帧 / 接收一帧”接口，向下屏蔽 ESP-IDF、STM32 HAL 和 POSIX（空桩）三种实现的差异。

源码：

- `pwos-shared/mesh/transport/mesh_uart_transport.c`
- `pwos-shared/mesh/transport/mesh_uart_transport.h`

## 2. 三平台编译选择

通过编译宏选择实现：

| 平台 | 宏 |
|------|-----|
| ESP-IDF | `ESP_PLATFORM` |
| STM32 HAL | `MESH_UART_TRANSPORT_USE_STM32_HAL` |
| POSIX / 其他 | 无上述宏 |

配置结构 `struct mesh_uart_transport_config`（`mesh_uart_transport.h:33`）在不同平台下字段不同：

- ESP-IDF：`uart_port`、`tx_pin`、`rx_pin`、`baud_rate`、`rx_buffer_size`、`tx_buffer_size`。
- STM32 HAL：`UART_HandleTypeDef *uart`。
- 其他：`void *uart`（空桩不使用）。

公共字段：

- `io_timeout_ms`：读写超时。
- `flush_before_receive`：接收前是否清空输入缓冲（调试开关）。

## 3. 统一 API

| 接口 | 说明 |
|------|------|
| `mesh_uart_transport_get_default_config()` | 填充默认配置。 |
| `mesh_uart_transport_init()` | 初始化对应平台的 UART/DMA。 |
| `mesh_uart_transport_deinit()` | 去初始化。 |
| `mesh_uart_transport_send_frame()` | 发送完整 mesh 帧。 |
| `mesh_uart_transport_receive_frame()` | 接收一帧完整 mesh 帧。 |
| `mesh_uart_transport_rx_pending()` | 查询是否有待处理帧。 |
| `mesh_uart_transport_flush_input()` | 清空输入状态。 |
| `mesh_uart_transport_get_stats()` | 获取运行统计。 |

`send_frame` 与 `receive_frame` 的回调签名与 `mesh_processer_send_frame_fn` / `mesh_processer_receive_frame_fn` 一致，可直接注入 `mesh_processer` 与 runtime。

## 4. STM32 HAL 实现：DMA + IDLE 中断 + Frame Queue

### 4.1 总体设计

STM32 路径使用 `HAL_UARTEx_ReceiveToIdle_DMA` 启动循环 DMA 接收，配合 UART IDLE 中断完成异步收包，避免轮询阻塞。

关键常量（`mesh_uart_transport.h:28`）：

```c
#define MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE 1024u
#define MESH_UART_TRANSPORT_STM32_FRAME_QUEUE_CAP    4u
#define MESH_UART_TRANSPORT_STM32_FRAME_CAP          \
    (MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN)
```

### 4.2 数据流

```
UART RX ──► DMA ──► dma_rx_buffer[1024]
                         │
                IDLE 中断 / RXEventCallback
                         │
              stm32_dma_consume_until_from_isr()
                         │
              逐字节状态机解析出完整帧
                         │
              stm32_enqueue_frame_from_isr()
                         │
              frame_queue[4]
                         │
              mesh_uart_transport_receive_frame()
```

### 4.3 帧解析状态机

在 ISR 上下文逐字节消费 DMA 数据：

1. 等待 `'M'`。
2. 等待 `'H'`。
3. 读取 `FrameLen` 字段，校验最小/最大长度。
4. 收齐整帧后压入 `frame_queue`。

异常处理：

- 非法长度或超限时增加 `bad_frames` 计数并复位解析器。
- 帧队列满时增加 `dropped_frames` 计数并丢弃该帧。

### 4.4 中断回调注册

通过全局表 `g_stm32_transports`（最多 8 实例）把 `UART_HandleTypeDef*` 映射到 transport 对象：

- `HAL_UARTEx_RxEventCallback()`：DMA 半传输/IDLE 事件，消费到当前 DMA 位置。
- `HAL_UART_ErrorCallback()`：出错后重启 DMA 接收。

## 5. ESP-IDF 实现

ESP-IDF 路径使用 `driver/uart.h`：

1. `uart_param_config()` 配置波特率、数据位、校验、停止位、流控。
2. `uart_driver_install()` 安装驱动并分配 ring buffer。
3. `uart_set_pin()` 设置 TX/RX 引脚。
4. 发送使用 `uart_write_bytes()` + `uart_wait_tx_done()`。
5. 接收使用 `uart_read_bytes()` 按字节精确读取帧头长度，再读取剩余字节。

收发各使用一把 FreeRTOS 互斥锁，保证线程安全。

默认参数（`mesh_uart_transport.h:21`）：

- `uart_port = 1`
- `tx_pin = 17`, `rx_pin = 18`
- `baud_rate = 1000000`
- `rx/tx_buffer_size = 1024`

## 6. POSIX 空桩

当既无 `ESP_PLATFORM` 也无 `MESH_UART_TRANSPORT_USE_STM32_HAL` 时，提供空桩实现：

- `init` 仅标记 initialized。
- `send_frame` 返回 `-MESH_ERR_INVALID_STATE`。
- `receive_frame` 返回 `-MESH_ERR_INVALID_STATE` 或 `-MESH_ERR_BUSY`。

PC 主控模拟器 `tools/pc_master_emulator/mesh_uart_transport_stub.c` 会覆盖这些空桩，用真实 POSIX 串口实现。

## 7. receive_frame 语义

`receive_frame` 的核心语义是：**非阻塞地返回一帧完整 mesh 帧**。

- STM32：从 `frame_queue` 出队；若无帧返回 `-MESH_ERR_BUSY`。
- ESP-IDF：从 UART ring buffer 精确读取帧头，解析长度后再读取剩余字节；无足够数据返回 `-MESH_ERR_BUSY`。
- 空桩：返回错误，表示无真实链路。

调用方（`mesh_node_service` / `mesh_host_service`）通常以轮询方式反复调用，软错误视为“暂无帧”。

## 8. rx stats

`struct mesh_uart_transport_stats`（`mesh_uart_transport.h:50`）字段：

| 字段 | 说明 |
|------|------|
| `initialized` | 是否初始化。 |
| `dma_running` | DMA 是否运行（仅 STM32）。 |
| `dma_pos` / `dma_last_pos` | 当前 DMA 写位置 / 上次消费位置。 |
| `parse_len` | 当前解析缓冲已收字节数。 |
| `frame_head` / `frame_count` | 帧队列头指针与当前帧数。 |
| `dropped_frames` | 队列满丢弃数。 |
| `bad_frames` | 非法帧计数。 |
| `hal_error_code` / `hal_g_state` / `hal_rx_state` | HAL 错误与状态（仅 STM32）。 |

从机 `/sys/uart` 通过 `mesh_node_service_format_uart_stats()` 输出这些字段。

## 9. 端口选择器如何映射到物理 UART / WiFi

`mesh_uart_transport` 本身只负责**单个 UART 端口**的字节收发。多端口映射由上层 service 完成：

- `mesh_node_service` 维护 `addr -> port_id` 表，把 cluster 返回的 mesh 地址映射到具体 UART 索引。
- `mesh_host_service` 使用静态 `neighbor_addr` 配置（单端口时通配，多端口时按地址匹配）。
- WiFi 保留位 `MESH_PORT_SELECTOR_WIFI_ID`（bit 7）在协议层声明；实际 TCP 链路由 `mesh_wifi_link` 提供，不经过 `mesh_uart_transport`。

## 10. send_frame / receive_frame 回调契约

### send_frame

```c
int send_frame(void *transport_ctx, uint8_t next_hop,
               const uint8_t *tx_data, size_t tx_len);
```

- `transport_ctx`：service 或 transport 实例指针。
- `next_hop`：cluster 路由返回的下一跳 mesh 地址；service 负责将其解析为物理端口。
- `tx_data` / `tx_len`：完整 mesh 帧（含 Magic、CRC）。
- 返回值：`0` 成功；负 `MESH_ERR_*` 失败。

### receive_frame

```c
int receive_frame(void *transport_ctx, uint8_t *rx_data, size_t rx_cap,
                  size_t *rx_len, uint8_t *out_ingress_port);
```

- `rx_data` / `rx_cap`：输出缓冲。
- `rx_len`：输出实际帧长。
- `out_ingress_port`：输出入口端口索引；无端口概念时填 `MESH_PROCESSER_INGRESS_PORT_NONE`。
- 返回值：`0` 收到一帧；`-MESH_ERR_BUSY` 表示暂无帧，可继续轮询。

## 11. 相关文件

- `pwos-shared/mesh/transport/mesh_uart_transport.c/.h`
- `pwos-master-esp32p4/mesh/mesh_host_service.c/.h`：主机侧多端口装配。
- `pwos-slave/User/mesh/mesh_node_service.c/.h`：从机侧多端口装配。
- `tools/pc_master_emulator/mesh_uart_transport_stub.c`：POSIX 串口桩。
