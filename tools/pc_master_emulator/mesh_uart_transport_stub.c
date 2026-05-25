#include "mesh_uart_transport.h"

#include <string.h>

void mesh_uart_transport_get_default_config(struct mesh_uart_transport_config *out_config)
{
    if (out_config != NULL) {
        memset(out_config, 0, sizeof(*out_config));
        out_config->io_timeout_ms = MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    }
}

int mesh_uart_transport_init(struct mesh_uart_transport *transport, const struct mesh_uart_transport_config *config)
{
    (void)config;
    if (transport == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    memset(transport, 0, sizeof(*transport));
    return -(int)MESH_ERR_INVALID_STATE;
}

void mesh_uart_transport_deinit(struct mesh_uart_transport *transport)
{
    if (transport != NULL) {
        memset(transport, 0, sizeof(*transport));
    }
}

int mesh_uart_transport_send_frame(struct mesh_uart_transport *transport, const uint8_t *tx_data, size_t tx_len)
{
    (void)transport;
    (void)tx_data;
    (void)tx_len;
    return -(int)MESH_ERR_INVALID_STATE;
}

int mesh_uart_transport_receive_frame(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    (void)transport;
    (void)rx_data;
    (void)rx_cap;
    if (rx_len != NULL) {
        *rx_len = 0u;
    }
    return -(int)MESH_ERR_INVALID_STATE;
}

int mesh_uart_transport_init_default(void)
{
    return -(int)MESH_ERR_INVALID_STATE;
}

struct mesh_uart_transport *mesh_uart_transport_default(void)
{
    return NULL;
}
