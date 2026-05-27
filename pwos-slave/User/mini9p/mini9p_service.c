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

static struct m9p_server g_mini9p_server;
static struct mesh_uart_transport g_mesh_uart_transports[MESH_NODE_RUNTIME_MAX_UART_PORTS];
static struct mesh_node_runtime g_mesh_node_runtime;
static struct mesh_node_runtime_port_config g_mesh_runtime_ports[MESH_NODE_RUNTIME_MAX_UART_PORTS];
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
    UART_HandleTypeDef *single_uart_list[1];
    UART_HandleTypeDef *const *uart_list;
    size_t uart_count;
    size_t i;
    int rc;

    if (backend == NULL || backend->ops == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    if (backend->uarts != NULL && backend->uart_count > 0u) {
        uart_list = backend->uarts;
        uart_count = backend->uart_count;
    } else if (backend->uart != NULL) {
        single_uart_list[0] = backend->uart;
        uart_list = single_uart_list;
        uart_count = 1u;
    } else {
        uart_list = NULL;
        uart_count = 0u;
    }
    if ((uart_count == 0u && !backend->wifi_supported) ||
        uart_count > MESH_NODE_RUNTIME_MAX_UART_PORTS) {
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

    memset(g_mesh_uart_transports, 0, sizeof(g_mesh_uart_transports));
    for (i = 0u; i < uart_count; ++i) {
        mesh_uart_transport_get_default_config(&uart_config);
        uart_config.uart = uart_list[i];
        uart_config.io_timeout_ms = MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
        uart_config.flush_before_receive = false;
        rc = mesh_uart_transport_init(&g_mesh_uart_transports[i], &uart_config);
        if (rc != 0) {
            while (i > 0u) {
                --i;
                mesh_uart_transport_deinit(&g_mesh_uart_transports[i]);
            }
            return rc;
        }
    }

    mesh_node_runtime_get_default_config(&runtime_config);
    memset(g_mesh_runtime_ports, 0, sizeof(g_mesh_runtime_ports));
    for (i = 0u; i < uart_count; ++i) {
        g_mesh_runtime_ports[i].send_frame = mini9p_service_send_frame;
        g_mesh_runtime_ports[i].receive_frame = mini9p_service_receive_frame;
        g_mesh_runtime_ports[i].transport_ctx = &g_mesh_uart_transports[i];
        g_mesh_runtime_ports[i].port_id = (uint8_t)i;
    }
    runtime_config.ports = g_mesh_runtime_ports;
    runtime_config.mini9p_server_handler = m9p_server_handle_frame;
    runtime_config.mini9p_server_ctx = &g_mini9p_server;
    runtime_config.wifi_config = backend->wifi_config;
    mini9p_service_fill_local_uid(runtime_config.local_uid);
    runtime_config.boot_nonce = mini9p_service_make_boot_nonce();
    runtime_config.capability_bits = MINI9P_SERVICE_REGISTER_CAPABILITY_BITS;
    runtime_config.port_bitmap = 0u;
    runtime_config.auto_register_on_init = true;
    rc = mesh_node_runtime_init(&g_mesh_node_runtime, &runtime_config, uart_count, backend->wifi_supported);
    if (rc != 0) {
        for (i = 0u; i < uart_count; ++i) {
            mesh_uart_transport_deinit(&g_mesh_uart_transports[i]);
        }
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
