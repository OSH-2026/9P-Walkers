# UART Transport 接口说明

## 模块定位

`uart_transport` 是 STM32 侧 mini9P 的 UART 传输层。

它现在同时支持两类模式：
- client 模式：主动发送一帧 request，再等待一帧 response
- server 模式：被动接收一帧 request，调用 handler 处理，再回发一帧 response

因此 STM32 不再只能做从机 server；如果上层需要，也可以直接把 `m9p_uart_transport_request` 挂给 `m9p_client`，主动向 ESP32 发起请求。

## 初始化

```c
void m9p_uart_transport_get_default_config(struct m9p_uart_transport_config *out_config);
int m9p_uart_transport_init(struct m9p_uart_transport *transport,
                            const struct m9p_uart_transport_config *config);
int m9p_uart_transport_init_default(void);
struct m9p_uart_transport *m9p_uart_transport_default(void);
```

默认配置使用 `huart2`：

```c
out_config->uart = &huart2;
out_config->io_timeout_ms = 200;
out_config->flush_before_request = false;
out_config->flush_before_receive = false;
```

## 原始收发接口

```c
int m9p_uart_transport_send_frame(struct m9p_uart_transport *transport,
                                  const uint8_t *tx_data,
                                  size_t tx_len);

int m9p_uart_transport_receive_frame(struct m9p_uart_transport *transport,
                                     uint8_t *rx_data,
                                     size_t rx_cap,
                                     size_t *rx_len);
```

这两层只负责“发完整一帧”和“收完整一帧”，不关心帧的业务语义。

## Client 入口

```c
int m9p_uart_transport_request(void *transport_ctx,
                               const uint8_t *tx_data,
                               size_t tx_len,
                               uint8_t *rx_data,
                               size_t rx_cap,
                               size_t *rx_len);
```

一次调用完成：

```text
send_frame
  -> receive_frame
```

这让 STM32 侧也可以直接复用主控侧 client 的调用模型。

## Server 入口

```c
int m9p_uart_transport_serve_once(struct m9p_uart_transport *transport,
                                  m9p_server_transport_fn handler,
                                  void *server_ctx,
                                  uint8_t *rx_data,
                                  size_t rx_cap,
                                  size_t *rx_len,
                                  uint8_t *tx_data,
                                  size_t tx_cap,
                                  size_t *tx_len);
```

一次调用处理一轮 request/response：

```text
receive_frame
  -> handler(server_ctx, request, response)
  -> send_frame
```

## Buffer 约定

调用方负责提供收发缓冲区：

```c
static uint8_t rx_buf[512];
static uint8_t tx_buf[512];
```

`rx_cap` 和 `tx_cap` 应至少不小于 `M9P_FRAME_OVERHEAD`。

`receive_frame()` 会先读取 4 字节头：

```text
'9' 'P' frame_len_le16
```

然后根据 `frame_len` 继续读完整帧：

```text
total_len = frame_len + 6
```

## 错误处理约定

transport 层返回负 mini9P 风格错误码：

```text
-M9P_ERR_EINVAL  参数错误
-M9P_ERR_EAGAIN  UART 超时
-M9P_ERR_EBUSY   transport 正在被其他事务占用
-M9P_ERR_EIO     UART 错误或帧头非法
-M9P_ERR_EMSIZE  缓冲区太小
```

注意：如果 `handler` 返回负数，`serve_once()` 不会自动发送 `Rerror`。因此协议级错误最好由 server 自己构造 `Rerror` 帧后返回成功。

## 典型调用方式

### 作为 server 使用
```c
while (1) {
    (void)m9p_uart_transport_serve_once(m9p_uart_transport_default(),
                                        m9p_server_handle_frame,
                                        &server,
                                        rx_buf,
                                        sizeof(rx_buf),
                                        NULL,
                                        tx_buf,
                                        sizeof(tx_buf),
                                        NULL);
}
```

### 作为 client 使用
```c
m9p_client_init(&client,
                m9p_uart_transport_request,
                m9p_uart_transport_default());
```

## Host 测试

新增 host 单元测试：`pwos-slave/User/mini9p/test/test_slave_uart_transport_host.c`

它通过 fake HAL UART 桩验证：
- `send_frame`
- `receive_frame`
- `request`
- `serve_once`

推荐本地编译命令：

```powershell
gcc -std=c11 -Wall -Wextra \
  -I. -I.\pwos-slave\User\mini9p -I.\pwos-slave\User\mini9p\test\stubs \
  .\pwos-slave\User\mini9p\test\test_slave_uart_transport_host.c \
  .\pwos-slave\User\mini9p\uart_transport.c \
  .\pwos-slave\User\mini9p\mini9p_protocol.c \
  -o .\build\test_slave_uart_transport_host.exe
```

## 当前限制

当前实现仍是 MVP：

```text
阻塞式 HAL UART
一次只处理一个事务
不支持 multi-tag 并发
坏帧后只返回 EIO，不做滑动重同步
```

后续如果引入 RTOS 或更高吞吐需求，可以再升级为中断/DMA 接收。
