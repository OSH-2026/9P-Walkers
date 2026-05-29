/**
 * @file mini9p_service.c
 * @brief STM32 slave 侧 Mesh + Mini9P 串口服务组装层。
 */

#include "mini9p_service.h"

#include <stdbool.h>
#include <string.h>

#include "mini9p_server.h"
#include "mesh_node_runtime.h"
#include "mesh_uart_transport.h"

/** 串口联调阶段的 RX/TX 帧缓冲区大小。 */
#define MINI9P_SERVICE_FRAME_CAP M9P_SERVER_DEFAULT_MSIZE
#define MINI9P_SERVICE_REGISTER_CAPABILITY_BITS 0x0001u
#define MINI9P_SERVICE_REGISTER_PORT_BITMAP 0x01u

static struct m9p_server g_mini9p_server;
static struct mesh_uart_transport g_mesh_uart_transport;
static struct mesh_node_runtime g_mesh_node_runtime;
static bool g_mini9p_service_initialized;

static void mini9p_service_store_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void mini9p_service_fill_local_uid(uint8_t out_uid[MESH_UID_LEN])
{
    uint32_t uid0 = HAL_GetUIDw0();
    uint32_t uid1 = HAL_GetUIDw1();
    uint32_t uid2 = HAL_GetUIDw2();

    mini9p_service_store_le32(out_uid, uid0);
    mini9p_service_store_le32(out_uid + 4u, uid1 ^ uid2);
}

static uint32_t mini9p_service_make_boot_nonce(void)
{
    return HAL_GetUIDw2() ^ HAL_GetTick();
}

static int mini9p_service_send_frame(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len)
{
    (void)next_hop;
    return mesh_uart_transport_send_frame((struct mesh_uart_transport *)transport_ctx, tx_data, tx_len);
}

static int mini9p_service_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    return mesh_uart_transport_receive_frame(
        (struct mesh_uart_transport *)transport_ctx,
        rx_data,
        rx_cap,
        rx_len);
}

int mini9p_service_init_with_backend(const struct mini9p_service_backend *backend)
{
    struct m9p_server_config server_config;
    struct mesh_uart_transport_config uart_config;
    struct mesh_node_runtime_config runtime_config;
    int rc;

    if (backend == NULL || backend->ops == NULL || backend->uart == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    m9p_server_get_default_config(&server_config);
    server_config.ops = backend->ops;
    server_config.ops_ctx = backend->ops_ctx;
    server_config.max_msize = MINI9P_SERVICE_FRAME_CAP;
    server_config.default_iounit = backend->default_iounit;
    rc = m9p_server_init(&g_mini9p_server, &server_config);
    if (rc < 0) {
        return rc;
    }

    mesh_uart_transport_get_default_config(&uart_config);
    uart_config.uart = backend->uart;
    uart_config.io_timeout_ms = MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    uart_config.flush_before_receive = false;
    rc = mesh_uart_transport_init(&g_mesh_uart_transport, &uart_config);
    if (rc != 0) {
        return rc;
    }

    mesh_node_runtime_get_default_config(&runtime_config);
    runtime_config.send_frame = mini9p_service_send_frame;
    runtime_config.receive_frame = mini9p_service_receive_frame;
    runtime_config.transport_ctx = &g_mesh_uart_transport;
    runtime_config.mini9p_server_handler = m9p_server_handle_frame;
    runtime_config.mini9p_server_ctx = &g_mini9p_server;
    mini9p_service_fill_local_uid(runtime_config.local_uid);
    runtime_config.boot_nonce = mini9p_service_make_boot_nonce();
    runtime_config.capability_bits = MINI9P_SERVICE_REGISTER_CAPABILITY_BITS;
    runtime_config.port_bitmap = MINI9P_SERVICE_REGISTER_PORT_BITMAP;
    runtime_config.auto_register_on_init = true;
    rc = mesh_node_runtime_init(&g_mesh_node_runtime, &runtime_config);
    if (rc != 0) {
        mesh_uart_transport_deinit(&g_mesh_uart_transport);
        return rc;
    }

    g_mini9p_service_initialized = true;
    return 0;
}

int mini9p_service_notify_link_up(void)
{
    if (!g_mini9p_service_initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_runtime_notify_link_up(&g_mesh_node_runtime);
}

int mini9p_service_poll_once(void)
{
    if (!g_mini9p_service_initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_runtime_poll_once(&g_mesh_node_runtime);
}

/**
 * @file mini9p_board_service.c
 * @brief Initializes pwos-slave local VFS backends and injects them into Mini9P service.
 */

#include "mini9p_board_service.h"

#include <string.h>

#include "dev_vfs.h"
#include "fs_selftest.h"
#include "lfs_port.hpp"
#include "lfs_vfs.h"
#include "mini9p_service.h"
#include "node_vfs.h"
#include "sys_vfs.h"
#include "usart.h"

static struct lfs_vfs g_lfs_vfs;
static struct sys_vfs g_sys_vfs;
static struct dev_vfs g_dev_vfs;
static struct node_vfs g_node_vfs;
static FS_SelfTestReport g_fs_report;

int mini9p_board_service_init(void)
{
    struct sys_vfs_config sys_config;
    struct dev_vfs_config dev_config;
    struct lfs_vfs_config lfs_config;
    struct node_vfs_config node_config;
    struct mini9p_service_backend backend;
    struct lfs_vfs *active_lfs = NULL;
    int rc;

#ifndef PWOS_SKIP_LFS_MOUNT
    lfs_vfs_get_default_config(&lfs_config);
    rc = lfs_vfs_init(&g_lfs_vfs, &lfs_config);
    if (rc == 0) {
#ifdef PWOS_ENABLE_LFS_SELFTEST
        (void)fs_selftest_run_on_fs(lfs_vfs_fs(&g_lfs_vfs), lfs_port_backend_name(), &g_fs_report);
#endif
        active_lfs = &g_lfs_vfs;
    }
#else
    (void)lfs_config;
#endif

    memset(&sys_config, 0, sizeof(sys_config));
    sys_config.info_text = active_lfs != NULL ? "pwos node lfs=ok\n" : "pwos node lfs=unavailable\n";
    sys_config.iounit = SYS_VFS_DEFAULT_IOUNIT;
    rc = sys_vfs_init(&g_sys_vfs, &sys_config);
    if (rc != 0) {
        return rc;
    }

    memset(&dev_config, 0, sizeof(dev_config));
    dev_config.iounit = DEV_VFS_DEFAULT_IOUNIT;
    rc = dev_vfs_init(&g_dev_vfs, &dev_config);
    if (rc != 0) {
        return rc;
    }

    memset(&node_config, 0, sizeof(node_config));
    node_config.sys_ops = sys_vfs_ops();
    node_config.sys_ctx = &g_sys_vfs;
    node_config.dev_ops = dev_vfs_ops();
    node_config.dev_ctx = &g_dev_vfs;
    if (active_lfs != NULL) {
        node_config.lfs_ops = lfs_vfs_ops();
        node_config.lfs_ctx = active_lfs;
    }
    node_config.iounit = NODE_VFS_DEFAULT_IOUNIT;
    rc = node_vfs_init(&g_node_vfs, &node_config);
    if (rc != 0) {
        return rc;
    }

    memset(&backend, 0, sizeof(backend));
    backend.ops = node_vfs_ops();
    backend.ops_ctx = &g_node_vfs;
    backend.default_iounit = g_node_vfs.iounit;
    backend.uart = &huart2;
    return mini9p_service_init_with_backend(&backend);
}

int mini9p_board_service_poll_once(void)
{
    return mini9p_service_poll_once();
}
