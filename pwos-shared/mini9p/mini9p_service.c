/**
 * @file mini9p_service.c
 * @brief STM32 slave 侧 Mini9P 串口服务组装层。
 */

#include "mini9p_service.h"

#include <stdbool.h>

#include "local_vfs.h"
#include "mini9p_peer_link.h"
#include "mini9p_server.h"
#include "uart_transport.h"

/** 串口联调阶段的 RX/TX 帧缓冲区大小。 */
#define MINI9P_SERVICE_FRAME_CAP M9P_SERVER_DEFAULT_MSIZE

static struct local_vfs g_local_vfs;
static struct m9p_peer_link g_mini9p_peer_link;
static struct m9p_server g_mini9p_server;
static uint8_t g_mini9p_rx[MINI9P_SERVICE_FRAME_CAP];
static uint8_t g_mini9p_tx[MINI9P_SERVICE_FRAME_CAP];
static bool g_mini9p_service_initialized;

static int mini9p_service_send_frame(void *transport_ctx, const uint8_t *tx_data, size_t tx_len)
{
    return m9p_uart_transport_send_frame((struct m9p_uart_transport *)transport_ctx, tx_data, tx_len);
}

static int mini9p_service_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    return m9p_uart_transport_receive_frame(
        (struct m9p_uart_transport *)transport_ctx,
        rx_data,
        rx_cap,
        rx_len);
}

int mini9p_service_init(void)
{
    struct local_vfs_config vfs_config;
    struct m9p_peer_link_config peer_link_config;
    struct m9p_server_config server_config;
    int rc;

    local_vfs_get_default_config(&vfs_config);
    rc = local_vfs_init(&g_local_vfs, &vfs_config);
    if (rc < 0) {
        return rc;
    }

    m9p_server_get_default_config(&server_config);
    server_config.ops = local_vfs_ops();
    server_config.ops_ctx = &g_local_vfs;
    server_config.max_msize = MINI9P_SERVICE_FRAME_CAP;
    server_config.default_iounit = g_local_vfs.iounit;
    rc = m9p_server_init(&g_mini9p_server, &server_config);
    if (rc < 0) {
        return rc;
    }

    rc = m9p_uart_transport_init_default();
    if (rc < 0) {
        return rc;
    }

    m9p_peer_link_get_default_config(&peer_link_config);
    peer_link_config.send_frame = mini9p_service_send_frame;
    peer_link_config.receive_frame = mini9p_service_receive_frame;
    peer_link_config.transport_ctx = m9p_uart_transport_default();
    peer_link_config.request_handler = m9p_server_handle_frame;
    peer_link_config.request_handler_ctx = &g_mini9p_server;
    peer_link_config.dispatch_rx_buffer = g_mini9p_rx;
    peer_link_config.dispatch_rx_cap = sizeof(g_mini9p_rx);
    peer_link_config.dispatch_tx_buffer = g_mini9p_tx;
    peer_link_config.dispatch_tx_cap = sizeof(g_mini9p_tx);
    rc = m9p_peer_link_init(&g_mini9p_peer_link, &peer_link_config);
    if (rc < 0) {
        return rc;
    }

    g_mini9p_service_initialized = true;
    return 0;
}

int mini9p_service_poll_once(void)
{
    if (!g_mini9p_service_initialized) {
        return -(int)M9P_ERR_EINVAL;
    }

    return m9p_peer_link_poll_once(&g_mini9p_peer_link);
}
