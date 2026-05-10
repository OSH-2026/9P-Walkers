#ifndef PWOS_SLAVE_UART_TRANSPORT_H
#define PWOS_SLAVE_UART_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"
#include "main.h"

#define M9P_UART_TRANSPORT_DEFAULT_TIMEOUT_MS 200u

struct m9p_uart_transport_config {
    UART_HandleTypeDef *uart;
    uint32_t io_timeout_ms;
    bool flush_before_receive;
};

struct m9p_uart_transport {
    struct m9p_uart_transport_config config;
    bool initialized;
};

typedef int (*m9p_server_transport_fn)(
    void *server_ctx,
    const uint8_t *request_data,
    size_t request_len,
    uint8_t *response_data,
    size_t response_cap,
    size_t *response_len);

void m9p_uart_transport_get_default_config(struct m9p_uart_transport_config *out_config);
int m9p_uart_transport_init(struct m9p_uart_transport *transport,
                            const struct m9p_uart_transport_config *config);
void m9p_uart_transport_deinit(struct m9p_uart_transport *transport);

int m9p_uart_transport_receive_frame(struct m9p_uart_transport *transport,
                                     uint8_t *rx_data,
                                     size_t rx_cap,
                                     size_t *rx_len);
int m9p_uart_transport_send_frame(struct m9p_uart_transport *transport,
                                  const uint8_t *tx_data,
                                  size_t tx_len);
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
