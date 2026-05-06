# UART Transport Layer for mini9P Client

本模块实现了 mini9P 协议客户端与底层 UART 硬件之间的传输适配层，服务对象是 ESP32 主控侧的 mini9P client。它负责把主控发往 STM32 从机的 mini9P 请求帧发送到 UART，并接收从机返回的完整响应帧。

需要先明确三个概念：
- 系统主控：ESP32
- 系统从机：STM32
- 外部仿真环境：PC，仅用于在非 ESP32 平台上做接口级验证

本模块中的条件编译只是在区分“ESP32 实机平台”和“非 ESP32 仿真平台”，不是在区分系统里的主从角色。

## 目录结构
- uart_transport.h：接口声明与配置结构体定义
- uart_transport.c：实现，包含 ESP32 平台和主机桩代码

## 设计目标
- **角色清晰**：ESP32 始终是主控侧调用者，STM32 始终是对端从机；PC 仅承担外部仿真作用。
- **平台区分明确**：通过 `#ifdef ESP_PLATFORM` 区分 ESP32 实现与非 ESP32 桩实现，便于在 PC 上做接口验证。
- **线程安全**：ESP32 下使用互斥锁保护 UART 事务，防止多线程竞争。
- **完整帧收发**：保证 mini9P 帧完整性，处理超时、错误码映射。
- **易于集成**：接口与 mini9P 客户端解耦，便于在 shell、VFS、cluster_config 等模块中直接使用。

## 主要接口

### 配置结构体
```c
struct m9p_uart_transport_config {
    int uart_port;           // UART 端口号（ESP32 驱动编号）
    int tx_pin;              // TX 引脚编号
    int rx_pin;              // RX 引脚编号
    int baud_rate;           // 波特率
    uint32_t io_timeout_ms;  // 单次收发超时（毫秒）
    size_t rx_buffer_size;   // UART 驱动接收缓冲区大小
    size_t tx_buffer_size;   // UART 驱动发送缓冲区大小
    bool flush_before_request; // 每次请求前是否清空输入缓冲
};
```

### 传输上下文结构体
```c
struct m9p_uart_transport {
    struct m9p_uart_transport_config config; // 当前配置
    void *lock;                             // 互斥锁（仅 ESP32）
    bool initialized;                       // 是否已初始化
};
```

### 初始化与反初始化
- `void m9p_uart_transport_get_default_config(struct m9p_uart_transport_config *out_config);`
  - 获取一份默认配置。
- `int m9p_uart_transport_init(struct m9p_uart_transport *transport, const struct m9p_uart_transport_config *config);`
  - 初始化 UART 传输上下文。
- `void m9p_uart_transport_deinit(struct m9p_uart_transport *transport);`
  - 反初始化，释放资源。
- `int m9p_uart_transport_init_default(void);`
  - 初始化全局默认传输实例。
- `struct m9p_uart_transport *m9p_uart_transport_default(void);`
  - 获取全局默认传输实例指针。

### mini9P 传输接口
- `int m9p_uart_transport_request(void *transport_ctx, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data, size_t rx_cap, size_t *rx_len);`
  - 由 ESP32 主控侧发送一帧 mini9P 请求到 UART 对端，并等待 STM32 从机返回完整响应。
  - 参数：
    - `transport_ctx`：`struct m9p_uart_transport *` 指针
    - `tx_data`/`tx_len`：待发送帧
    - `rx_data`/`rx_cap`：接收缓冲区及容量
    - `rx_len`：实际接收长度
  - 返回值：0 成功，负数为错误码（见 mini9p_client.h）

## 平台适配说明
- **ESP32 主控实机**：调用 ESP-IDF UART 驱动，负责和 STM32 从机进行真实串口通信，支持多线程、超时、完整帧收发。
- **非 ESP32 外部仿真环境（如 PC）**：接口退化为桩实现，`request` 返回 ENOTSUP，用于验证调用边界、初始化路径和错误处理分支。

## 用法示例
```c
#include "uart_transport.h"
#include "mini9p_client.h"

struct m9p_uart_transport transport;
struct m9p_uart_transport_config config;
m9p_uart_transport_get_default_config(&config);
m9p_uart_transport_init(&transport, &config);

struct m9p_client client;
m9p_client_init(&client, m9p_uart_transport_request, &transport);
// ...
m9p_uart_transport_deinit(&transport);
```

## PC 外部仿真 testbench
- 由于非 ESP32 平台上的 `m9p_uart_transport_request` 是桩实现，因此这个 testbench 只能验证 transport 接口边界，而不能模拟真实的 ESP32<->STM32 串口往返。
- 如果目标是验证 mini9P 协议行为或 VFS 行为，仍应使用 mini9p_client 的 mock transport 或现有 VFS testbench。
- 若需验证接口可用性，可编写如下最小 testbench：

```c
#include "uart_transport.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct m9p_uart_transport transport;
    struct m9p_uart_transport_config config;
    m9p_uart_transport_get_default_config(&config);
    int ret = m9p_uart_transport_init(&transport, &config);
    assert(ret == 0);
    uint8_t rx[16];
    size_t rx_len = 0;
    ret = m9p_uart_transport_request(&transport, (const uint8_t*)"test", 4, rx, sizeof(rx), &rx_len);
    assert(ret < 0); // 非 ESP32 平台下应返回 ENOTSUP
    printf("request ret=%d\n", ret);
    m9p_uart_transport_deinit(&transport);
    return 0;
}
```

  这段 testbench 的意义是确认：
  - 头文件和实现文件可以在非 ESP32 环境下正常编译
  - 初始化和反初始化接口的参数约束成立
  - `request` 在仿真分支中稳定返回“不支持”的预期错误码

  它不验证以下内容：
  - ESP32 主控 UART 驱动配置是否正确
  - STM32 从机是否按 mini9P 返回合法响应
  - 帧头、长度和超时逻辑在真实串口上的行为

## 详细注释
- 详见 uart_transport.c，每个函数均有详细注释说明参数、行为和平台差异。
