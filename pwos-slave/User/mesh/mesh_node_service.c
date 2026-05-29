/**
 * @file mesh_node_service.c
 * @brief STM32 slave 侧 Mesh 节点服务装配层。
 */

#include "mesh_node_service.h"

#include <string.h>

#define MESH_NODE_SERVICE_DEFAULT_LOCAL_ADDR MESH_ADDR_UNASSIGNED

static struct mesh_uart_transport g_mesh_uart_transport;
static struct mesh_node_runtime g_mesh_node_runtime;
static bool g_mesh_node_service_initialized;

static void mesh_node_service_store_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void mesh_node_service_fill_local_uid(uint8_t out_uid[MESH_UID_LEN])
{
    uint32_t uid0 = HAL_GetUIDw0();
    uint32_t uid1 = HAL_GetUIDw1();
    uint32_t uid2 = HAL_GetUIDw2();

    mesh_node_service_store_le32(out_uid, uid0);
    mesh_node_service_store_le32(out_uid + 4u, uid1 ^ uid2);
}

static uint32_t mesh_node_service_make_boot_nonce(void)
{
    return HAL_GetUIDw2() ^ HAL_GetTick();
}

static int mesh_node_service_send_frame(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len)
{
    (void)next_hop;
    return mesh_uart_transport_send_frame((struct mesh_uart_transport *)transport_ctx, tx_data, tx_len);
}

static int mesh_node_service_receive_frame(
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

void mesh_node_service_get_default_config(struct mesh_node_service_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->capability_bits = MESH_NODE_SERVICE_REGISTER_CAPABILITY_BITS;
    out_config->port_bitmap = MESH_NODE_SERVICE_REGISTER_PORT_BITMAP;
    out_config->auto_register_on_init = true;
}

int mesh_node_service_init(const struct mesh_node_service_config *config)
{
    struct mesh_uart_transport_config uart_config;
    struct mesh_node_runtime_config runtime_config;
    int rc;

    if (config == NULL || config->uart == NULL || config->mini9p_server_handler == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    if (g_mesh_node_service_initialized) {
        mesh_node_service_deinit();
    }

    mesh_uart_transport_get_default_config(&uart_config);
    uart_config.uart = config->uart;
    uart_config.io_timeout_ms = MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    uart_config.flush_before_receive = false;
    rc = mesh_uart_transport_init(&g_mesh_uart_transport, &uart_config);
    if (rc != 0) {
        return rc;
    }

    mesh_node_runtime_get_default_config(&runtime_config);
    runtime_config.send_frame = mesh_node_service_send_frame;
    runtime_config.receive_frame = mesh_node_service_receive_frame;
    runtime_config.transport_ctx = &g_mesh_uart_transport;
    runtime_config.mini9p_server_handler = config->mini9p_server_handler;
    runtime_config.mini9p_server_ctx = config->mini9p_server_ctx;
    mesh_node_service_fill_local_uid(runtime_config.local_uid);
    runtime_config.boot_nonce = mesh_node_service_make_boot_nonce();
    runtime_config.capability_bits = config->capability_bits;
    runtime_config.port_bitmap = config->port_bitmap;
    runtime_config.local_addr = MESH_NODE_SERVICE_DEFAULT_LOCAL_ADDR;
    runtime_config.auto_register_on_init = config->auto_register_on_init;
    rc = mesh_node_runtime_init(&g_mesh_node_runtime, &runtime_config);
    if (rc != 0) {
        mesh_uart_transport_deinit(&g_mesh_uart_transport);
        return rc;
    }

    g_mesh_node_service_initialized = true;
    return 0;
}

void mesh_node_service_deinit(void)
{
    mesh_node_runtime_deinit(&g_mesh_node_runtime);
    mesh_uart_transport_deinit(&g_mesh_uart_transport);
    g_mesh_node_service_initialized = false;
}

int mesh_node_service_notify_link_up(void)
{
    if (!g_mesh_node_service_initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_runtime_notify_link_up(&g_mesh_node_runtime);
}

int mesh_node_service_poll_once(void)
{
    if (!g_mesh_node_service_initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_runtime_poll_once(&g_mesh_node_runtime);
}

struct mesh_node_runtime *mesh_node_service_runtime(void)
{
    if (!g_mesh_node_service_initialized) {
        return NULL;
    }

    return &g_mesh_node_runtime;
}
