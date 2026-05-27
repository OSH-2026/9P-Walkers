#include "mesh_transport_manager.h"

#include <stdbool.h>
#include <string.h>

#include "mesh_uart_transport.h"

static struct mesh_uart_transport g_default_transport;
static bool g_default_transport_initialized;

int mesh_transport_manager_init_default(void)
{
    struct mesh_uart_transport_config config;
    int rc;

    if (g_default_transport_initialized) {
        return 0;
    }

    mesh_uart_transport_get_default_config(&config);
    rc = mesh_uart_transport_init(&g_default_transport, &config);
    if (rc != 0) {
        memset(&g_default_transport, 0, sizeof(g_default_transport));
        return rc;
    }

    g_default_transport_initialized = true;
    return 0;
}

void mesh_transport_manager_deinit_default(void)
{
    if (!g_default_transport_initialized) {
        return;
    }

    mesh_uart_transport_deinit(&g_default_transport);
    g_default_transport_initialized = false;
}

void *mesh_transport_manager_default(void)
{
    if (!g_default_transport_initialized) {
        return NULL;
    }

    return &g_default_transport;
}

int mesh_transport_manager_send_frame(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len)
{
    (void)next_hop;
    return mesh_uart_transport_send_frame((struct mesh_uart_transport *)transport_ctx, tx_data, tx_len);
}

int mesh_transport_manager_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    return mesh_uart_transport_receive_frame((struct mesh_uart_transport *)transport_ctx, rx_data, rx_cap, rx_len);
}
