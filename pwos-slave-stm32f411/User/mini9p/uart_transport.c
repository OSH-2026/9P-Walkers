/*
 * STM32 侧系统角色是 mini9P 从机（server）。
 * 这份 transport 和 ESP32 主控侧保持相同的生命周期接口：
 * - get_default_config
 * - init / deinit
 * - init_default / default
 *
 * 但真正的数据流方向不同：
 * - 主控侧是 send request -> wait response
 * - 从机侧是 receive request -> process -> send response
 */
#include "uart_transport.h"

#include <limits.h>
#include <string.h>

static struct m9p_uart_transport g_default_transport;

static uint32_t transport_timeout_ms(const struct m9p_uart_transport *transport)
{
    if (transport->config.io_timeout_ms == 0u) {
        return 1u;
    }

    return transport->config.io_timeout_ms;
}

static int hal_status_to_m9p(HAL_StatusTypeDef status)
{
    switch (status) {
    case HAL_OK:
        return 0;
    case HAL_TIMEOUT:
        return -(int)M9P_ERR_EAGAIN;
    case HAL_BUSY:
        return -(int)M9P_ERR_EBUSY;
    case HAL_ERROR:
    default:
        return -(int)M9P_ERR_EIO;
    }
}

static int drain_rx_fifo(struct m9p_uart_transport *transport)
{
    uint8_t byte;

    while (HAL_UART_Receive(transport->config.uart, &byte, 1u, 1u) == HAL_OK) {
    }
    __HAL_UART_FLUSH_DRREGISTER(transport->config.uart);
    return 0;
}

static int read_exact(struct m9p_uart_transport *transport, uint8_t *buf, size_t len)
{
    size_t total = 0u;

    while (total < len) {
        size_t chunk = len - total;
        int ret;

        if (chunk > (size_t)UINT16_MAX) {
            chunk = (size_t)UINT16_MAX;
        }

        ret = hal_status_to_m9p(HAL_UART_Receive(transport->config.uart,
                                                 buf + total,
                                                 (uint16_t)chunk,
                                                 transport_timeout_ms(transport)));
        if (ret < 0) {
            return ret;
        }

        total += chunk;
    }

    return 0;
}

void m9p_uart_transport_get_default_config(struct m9p_uart_transport_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    out_config->uart = &huart2;
    out_config->io_timeout_ms = M9P_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    out_config->flush_before_receive = false;
}

int m9p_uart_transport_init(struct m9p_uart_transport *transport,
                            const struct m9p_uart_transport_config *config)
{
    if (transport == NULL || config == NULL || config->uart == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    transport->config = *config;
    transport->initialized = true;
    return 0;
}

void m9p_uart_transport_deinit(struct m9p_uart_transport *transport)
{
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
}

int m9p_uart_transport_receive_frame(struct m9p_uart_transport *transport,
                                     uint8_t *rx_data,
                                     size_t rx_cap,
                                     size_t *rx_len)
{
    uint8_t header[4];
    uint16_t frame_len_field;
    size_t total_len;
    int ret;

    if (transport == NULL || !transport->initialized || rx_data == NULL || rx_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    *rx_len = 0u;

    if (transport->config.flush_before_receive) {
        ret = drain_rx_fifo(transport);
        if (ret < 0) {
            return ret;
        }
    }

    ret = read_exact(transport, header, sizeof(header));
    if (ret < 0) {
        return ret;
    }
    if (header[0] != (uint8_t)'9' || header[1] != (uint8_t)'P') {
        return -(int)M9P_ERR_EIO;
    }

    frame_len_field = (uint16_t)header[2] | (uint16_t)((uint16_t)header[3] << 8);
    if (frame_len_field < 4u) {
        return -(int)M9P_ERR_EIO;
    }

    total_len = (size_t)frame_len_field + 6u;
    if (total_len > rx_cap) {
        return -(int)M9P_ERR_EMSIZE;
    }

    memcpy(rx_data, header, sizeof(header));
    ret = read_exact(transport, rx_data + sizeof(header), total_len - sizeof(header));
    if (ret < 0) {
        return ret;
    }

    *rx_len = total_len;
    return 0;
}

int m9p_uart_transport_send_frame(struct m9p_uart_transport *transport,
                                  const uint8_t *tx_data,
                                  size_t tx_len)
{
    if (transport == NULL || !transport->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (tx_len > (size_t)UINT16_MAX) {
        return -(int)M9P_ERR_EMSIZE;
    }

    return hal_status_to_m9p(HAL_UART_Transmit(transport->config.uart,
                                               (const uint8_t *)tx_data,
                                               (uint16_t)tx_len,
                                               transport_timeout_ms(transport)));
}

int m9p_uart_transport_serve_once(struct m9p_uart_transport *transport,
                                  m9p_server_transport_fn handler,
                                  void *server_ctx,
                                  uint8_t *rx_data,
                                  size_t rx_cap,
                                  size_t *rx_len,
                                  uint8_t *tx_data,
                                  size_t tx_cap,
                                  size_t *tx_len)
{
    size_t local_rx_len = 0u;
    size_t local_tx_len = 0u;
    int ret;

    if (transport == NULL || handler == NULL || rx_data == NULL || tx_data == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    ret = m9p_uart_transport_receive_frame(transport, rx_data, rx_cap, &local_rx_len);
    if (ret < 0) {
        return ret;
    }

    ret = handler(server_ctx, rx_data, local_rx_len, tx_data, tx_cap, &local_tx_len);
    if (ret < 0) {
        return ret;
    }
    if (local_tx_len == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    ret = m9p_uart_transport_send_frame(transport, tx_data, local_tx_len);
    if (ret < 0) {
        return ret;
    }

    if (rx_len != NULL) {
        *rx_len = local_rx_len;
    }
    if (tx_len != NULL) {
        *tx_len = local_tx_len;
    }

    return 0;
}

int m9p_uart_transport_init_default(void)
{
    struct m9p_uart_transport_config config;

    if (g_default_transport.initialized) {
        return 0;
    }

    m9p_uart_transport_get_default_config(&config);
    return m9p_uart_transport_init(&g_default_transport, &config);
}

struct m9p_uart_transport *m9p_uart_transport_default(void)
{
    return &g_default_transport;
}