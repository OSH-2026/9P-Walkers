# UART Transport Layer for mini9P

本模块实现了 ESP32 主控侧的 mini9P UART 传输层。

当前它不再只服务 mini9P client，而是同时支持两种事务模型：
- client 模式：主动发送 request，再等待 response
- server 模式：被动接收 request，调用 handler 处理，再回发 response

需要先明确三个概念：
- 系统主控：ESP32
- 系统从机：STM32
- 外部仿真环境：PC，仅用于 host 单元测试和接口验证

本模块中的条件编译只是在区分“ESP32 实机平台”和“非 ESP32 仿真平台”，不是在区分系统里的主从角色。

## 设计目标
- 角色清晰：ESP32 和 STM32 都可以同时挂 client/server，UART transport 不再把自己绑定死到单一角色。
- 平台区分明确：通过 `#ifdef ESP_PLATFORM` 区分 ESP32 实现与 host 桩实现。
- 收发分离：ESP32 下把 TX 和 RX 拆成独立互斥锁，raw API 可以按 UART 物理全双工方式并行收发。
- 事务边界清晰：`request` / `serve_once` 仍是单事务 helper，会额外拿整轮 exchange 锁，防止一轮 RPC 被打散。
- 完整帧收发：统一通过“先读 4 字节头，再按长度补齐剩余部分”的方式处理完整 mini9P 帧。

## 主要接口

### 配置结构体
```c
struct m9p_uart_transport_config {
    int uart_port;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    uint32_t io_timeout_ms;
    size_t rx_buffer_size;
    size_t tx_buffer_size;
    bool flush_before_request;
    bool flush_before_receive;
};
```

### 生命周期接口
- `m9p_uart_transport_get_default_config`
- `m9p_uart_transport_init`
- `m9p_uart_transport_deinit`
- `m9p_uart_transport_init_default`
- `m9p_uart_transport_default`

### 原始收发接口
- `m9p_uart_transport_send_frame`
  - 只负责发送一整帧，不等待响应。
  - 只占用 TX 方向，可以和 `receive_frame` 并行使用。
- `m9p_uart_transport_receive_frame`
  - 只负责接收一整帧，不调用协议处理器。
  - 只占用 RX 方向，可以和 `send_frame` 并行使用。

### client 模式接口
- `m9p_uart_transport_request`
  - 发送一帧请求，再等待一帧响应。
  - 这是 `m9p_client` 直接需要的 transport 回调形式。
  - 该 helper 会同时占住 TX/RX/exchange 三把锁，因此仍按“单事务”运行。

### server 模式接口
- `m9p_uart_transport_serve_once`
  - 接收一帧请求，调用 handler 生成响应，再把响应发回 UART。
  - 这样 ESP32 侧如果未来要承接 STM32 的反向请求，不必重写第二套 UART 事务逻辑。
  - 该 helper 同样是单事务包装，不是多路复用 dispatcher。

## 用法示例

### 作为 client 使用
```c
struct m9p_uart_transport transport;
struct m9p_uart_transport_config config;
struct m9p_client client;

m9p_uart_transport_get_default_config(&config);
m9p_uart_transport_init(&transport, &config);
m9p_client_init(&client, m9p_uart_transport_request, &transport);
```

### 作为 server 使用
```c
static int server_handler(void *server_ctx,
                          const uint8_t *request_data,
                          size_t request_len,
                          uint8_t *response_data,
                          size_t response_cap,
                          size_t *response_len);

uint8_t rx_buf[512];
uint8_t tx_buf[512];

m9p_uart_transport_serve_once(&transport,
                              server_handler,
                              server_ctx,
                              rx_buf,
                              sizeof(rx_buf),
                              NULL,
                              tx_buf,
                              sizeof(tx_buf),
                              NULL);
```

## Host 测试

### 轻量桩测试
- `pwos-master-esp32p4/transport/testbench_uart_transport.c`
  - 验证非 ESP32 分支下的接口边界。

### 完整 host 单元测试
- `pwos-master-esp32p4/transport/test/test_master_uart_transport_host.c`
  - 通过 fake UART 和 fake FreeRTOS/ESP-IDF stub 验证：
  - `send_frame`
  - `receive_frame`
  - `request`
  - `serve_once`
  - TX 锁占用不阻塞 raw RX，RX 锁占用不阻塞 raw TX

推荐本地编译命令：

```powershell
gcc -std=c11 -Wall -Wextra -DESP_PLATFORM \
  -I. -I.\pwos-master-esp32p4\transport -I.\pwos-shared\mini9p \
  -I.\pwos-master-esp32p4\transport\test\stubs \
  -I.\pwos-master-esp32p4\transport\test\stubs\driver \
  -I.\pwos-master-esp32p4\transport\test\stubs\freertos \
  .\pwos-master-esp32p4\transport\test\test_master_uart_transport_host.c \
  .\pwos-master-esp32p4\transport\uart_transport.c \
  .\pwos-shared\mini9p\mini9p_protocol.c \
  -o .\build\test_master_uart_transport_host.exe
```

## 详细注释
- 详见 `uart_transport.c` 和 `uart_transport.h`，当前已经补齐到逐函数、逐分支说明的粒度。

## 当前边界
- 物理 UART 与 raw frame API 现在按全双工资源模型工作。
- `request` / `serve_once` 仍是单事务 helper，不支持 multi-tag 并发。
- 如果要做到“本端发起请求的同时，还能继续承接对端主动上报/请求”，当前工程不再使用独立 peer_link，而是把本模块放在 `mesh_uart_transport` 之下，由主机侧 `mesh_host_runtime` 或节点侧 `mesh_node_runtime` 负责继续收帧与分流。
- 当前 master 运行时的默认节点客户端已切到 `mesh_host_runtime_client_request()`：它会先把 Mini9P 帧封成 mesh 数据面，再在等待响应时继续处理 REGISTER、LINK_STATE、ROUTE_UPDATE 等途中帧。
