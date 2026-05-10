#ifndef PWOS_MASTER_UART_TRANSPORT_H
#define PWOS_MASTER_UART_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M9P_UART_TRANSPORT_DEFAULT_PORT 1
#define M9P_UART_TRANSPORT_DEFAULT_TX_PIN 17
#define M9P_UART_TRANSPORT_DEFAULT_RX_PIN 18
#define M9P_UART_TRANSPORT_DEFAULT_BAUD_RATE 1000000
#define M9P_UART_TRANSPORT_DEFAULT_TIMEOUT_MS 200u
#define M9P_UART_TRANSPORT_DEFAULT_RX_BUFFER_SIZE 1024u
#define M9P_UART_TRANSPORT_DEFAULT_TX_BUFFER_SIZE 1024u

typedef int (*m9p_server_transport_fn)(
    void *server_ctx,
    const uint8_t *request_data,
    size_t request_len,
    uint8_t *response_data,
    size_t response_cap,
    size_t *response_len);

struct m9p_uart_transport_config {
    int uart_port;               /* ESP32 里的 UART 编号，例如 UART1。 */
    int tx_pin;                  /* 串口发送引脚。 */
    int rx_pin;                  /* 串口接收引脚。 */
    int baud_rate;               /* 波特率，例如 115200 或 1000000。 */
    uint32_t io_timeout_ms;      /* 一次收/发等待多久算超时，单位毫秒。 */
    size_t rx_buffer_size;       /* UART 驱动内部接收缓冲区大小。 */
    size_t tx_buffer_size;       /* UART 驱动内部发送缓冲区大小。 */
    bool flush_before_request;   /* 主动发请求前，是否先丢掉旧残留数据。 */
    bool flush_before_receive;   /* 被动收帧前，是否先丢掉旧残留数据。 */
};

struct m9p_uart_transport {
    struct m9p_uart_transport_config config;  /* 当前实际生效的配置。 */
    void *tx_lock;                            /* 发送方向独立锁；允许 raw RX 与 raw TX 并行。 */
    void *rx_lock;                            /* 接收方向独立锁；允许 raw TX 与 raw RX 并行。 */
    void *exchange_lock;                      /* request/serve_once 使用的整轮事务锁。 */
    bool initialized;                         /* 是否已经完成初始化。 */
};

void m9p_uart_transport_get_default_config(struct m9p_uart_transport_config *out_config);
int m9p_uart_transport_init(struct m9p_uart_transport *transport,
                            const struct m9p_uart_transport_config *config);
void m9p_uart_transport_deinit(struct m9p_uart_transport *transport);

int m9p_uart_transport_send_frame(struct m9p_uart_transport *transport,
                                  const uint8_t *tx_data,
                                  size_t tx_len);
int m9p_uart_transport_receive_frame(struct m9p_uart_transport *transport,
                                     uint8_t *rx_data,
                                     size_t rx_cap,
                                     size_t *rx_len);
int m9p_uart_transport_request(void *transport_ctx,
                               const uint8_t *tx_data,
                               size_t tx_len,
                               uint8_t *rx_data,
                               size_t rx_cap,
                               size_t *rx_len);
int m9p_uart_transport_serve_once(struct m9p_uart_transport *transport,
                                  m9p_server_transport_fn handler,
                                  void *server_ctx,
                                  uint8_t *rx_data,
                                  size_t rx_cap,
                                  size_t *rx_len,
                                  uint8_t *tx_data,
                                  size_t tx_cap,
                                  size_t *tx_len);

int m9p_uart_transport_init_default(void);
struct m9p_uart_transport *m9p_uart_transport_default(void);

#endif /* PWOS_MASTER_UART_TRANSPORT_H */