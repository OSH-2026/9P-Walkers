#ifndef PWOS_SLAVE_UART_TRANSPORT_H
#define PWOS_SLAVE_UART_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"
#include "usart.h"

#define M9P_UART_TRANSPORT_DEFAULT_TIMEOUT_MS 200u

typedef int (*m9p_server_transport_fn)(
    void *server_ctx,
    const uint8_t *request_data,
    size_t request_len,
    uint8_t *response_data,
    size_t response_cap,
    size_t *response_len);

struct m9p_uart_transport_config {
    UART_HandleTypeDef *uart;   /* 当前 transport 绑定的 HAL UART 句柄，例如 &huart2。 */
    uint32_t io_timeout_ms;     /* 一次 HAL 收发最多等待多久，单位毫秒。 */
    bool flush_before_request;  /* 主动发请求前，是否先清掉 RX 里的旧碎片。 */
    bool flush_before_receive;  /* 被动收帧前，是否先清掉 RX 里的旧碎片。 */
};

struct m9p_uart_transport {
    struct m9p_uart_transport_config config;  /* 当前实际生效的配置。 */
    bool busy;                                /* 轻量重入保护，避免同一 UART 被嵌套复用。 */
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

#endif