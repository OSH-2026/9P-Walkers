/**
 * @file mini9p_service.c
 * @brief STM32 slave 侧 Mini9P 串口服务组装层。
 */

#include "mini9p_service.h"

#include <stdbool.h>

#include "local_vfs.h"
#include "mini9p_server.h"
#include "uart_transport.h"

/** 串口联调阶段的 RX/TX 帧缓冲区大小。 */
#define MINI9P_SERVICE_FRAME_CAP M9P_SERVER_DEFAULT_MSIZE

static struct local_vfs g_local_vfs;
static struct m9p_server g_mini9p_server;
static uint8_t g_mini9p_rx[MINI9P_SERVICE_FRAME_CAP];
static uint8_t g_mini9p_tx[MINI9P_SERVICE_FRAME_CAP];
static bool g_mini9p_service_initialized;

int mini9p_service_init(void)
{
    struct local_vfs_config vfs_config;
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

    g_mini9p_service_initialized = true;
    return 0;
}

int mini9p_service_poll_once(void)
{
    if (!g_mini9p_service_initialized) {
        return -(int)M9P_ERR_EINVAL;
    }

    return m9p_uart_transport_serve_once(m9p_uart_transport_default(),
                                         m9p_server_handle_frame,
                                         &g_mini9p_server,
                                         g_mini9p_rx,
                                         sizeof(g_mini9p_rx),
                                         NULL,
                                         g_mini9p_tx,
                                         sizeof(g_mini9p_tx),
                                         NULL);
}
