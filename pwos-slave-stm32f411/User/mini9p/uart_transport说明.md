# UART Transport 接口说明

## 模块定位

`uart_transport` 是 STM32 从机侧 mini9P 的传输层。

它只负责：

```text
从 UART 接收一帧完整 mini9P request
调用 server handler 处理请求
把 handler 生成的 response frame 发回 UART
```

它不负责：

```text
解析 Tattach/Tread 等协议语义
维护 fid 表
访问 littlefs 或虚拟节点
构造 Rread/Rerror 等响应内容
```

这些逻辑都属于 `mini9p_server`。

## 主要接口

### 初始化

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
out_config->flush_before_receive = false;
```

server 侧一般不建议开启 `flush_before_receive`，否则可能丢掉 master 已经发来的请求。

## Server 入口

核心接口是：

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

`handler` 的签名是：

```c
typedef int (*m9p_server_transport_fn)(
    void *server_ctx,
    const uint8_t *request_data,
    size_t request_len,
    uint8_t *response_data,
    size_t response_cap,
    size_t *response_len);
```

`mini9p_server` 应该提供兼容这个签名的函数，例如：

```c
int m9p_server_handle_frame(void *server_ctx,
                            const uint8_t *request_data,
                            size_t request_len,
                            uint8_t *response_data,
                            size_t response_cap,
                            size_t *response_len);
```

## Buffer 约定

调用方负责提供收发缓冲区：

```c
static uint8_t rx_buf[512];
static uint8_t tx_buf[512];
```

`rx_cap` 和 `tx_cap` 应至少大于 `M9P_FRAME_OVERHEAD`。

`receive_frame()` 会先读取 4 字节头：

```text
'9' 'P' frame_len_le16
```

然后根据 `frame_len` 继续读完整帧：

```text
total_len = frame_len + 6
```

这和 `m9p_encode_frame()` 的帧格式一致。

## 错误处理约定

transport 层返回负 mini9P 风格错误码：

```text
-M9P_ERR_EINVAL  参数错误
-M9P_ERR_EAGAIN  UART 超时
-M9P_ERR_EBUSY   UART busy
-M9P_ERR_EIO     UART 或帧头错误
-M9P_ERR_EMSIZE  缓冲区太小
```

注意：如果 `handler` 返回负数，`serve_once()` 不会自动发送 `Rerror`。

因此 server 遇到协议级错误时，应该尽量自己构造 `Rerror`，然后返回 `0`：

```text
非法 fid     -> Rerror EFID
非法路径    -> Rerror ENOENT
非法请求格式 -> Rerror EINVAL
```

只有无法构造响应、参数错误或内部严重错误时，handler 才应该返回负数。

## 典型调用方式

```c
static struct m9p_server server;
static uint8_t rx_buf[512];
static uint8_t tx_buf[512];

m9p_uart_transport_init_default();
m9p_server_init(&server);

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

## 当前限制

当前实现适合 MVP：

```text
阻塞式 HAL UART
一次只处理一个 request
不支持 multi-tag 并发
坏帧后只返回 EIO，不做滑动重同步
```

后续如果引入 RTOS 或更高吞吐需求，可以再升级为中断/DMA 接收。
