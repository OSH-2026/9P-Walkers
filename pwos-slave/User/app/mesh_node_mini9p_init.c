#include "mesh_node_mini9p_init.h"

#include "mesh_diag.h"
#include "mesh_node_service.h"
#include "mini9p_server.h"
#include "node_vfs.h"
#include "pwos_log.h"

#include <stdbool.h>
#include <string.h>

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
extern UART_HandleTypeDef huart4;
#ifdef PWOS_ENABLE_UART5_MESH
extern UART_HandleTypeDef huart5;
#endif
extern UART_HandleTypeDef huart6;

static struct node_vfs g_node_vfs;
static struct m9p_server g_mini9p_server;

static int mesh_node_routes_text(void *ctx, char *out, size_t out_cap)
{
    size_t used;
    int rc;

    (void)ctx;
    rc = mesh_node_runtime_format_routes(mesh_node_service_runtime(), out, out_cap);
    if (rc != 0) {
        return rc;
    }

    used = strlen(out);
    if (used >= out_cap) {
        return 0;
    }

    return mesh_node_service_format_addr_ports(out + used, out_cap - used);
}

static int mesh_node_log_text(void *ctx, char *out, size_t out_cap)
{
    (void)ctx;
    return pwos_log_format(out, out_cap);
}

static int mesh_node_uart_text(void *ctx, char *out, size_t out_cap)
{
    (void)ctx;
    return mesh_node_service_format_uart_stats(out, out_cap);
}

static bool mesh_node_uart_is_enabled(const UART_HandleTypeDef *uart)
{
    return uart != NULL &&
        uart->Instance != NULL &&
        HAL_UART_GetState(uart) != HAL_UART_STATE_RESET;
}

static int mesh_node_add_uart_port(
    struct mesh_node_service_config *mesh_config,
    UART_HandleTypeDef *uart)
{
    struct mesh_node_service_port_config *port;

    if (mesh_config == NULL || !mesh_node_uart_is_enabled(uart)) {
        return 0;
    }
    if (mesh_config->port_count >= MESH_NODE_SERVICE_MAX_PORTS) {
        return -(int)MESH_ERR_BUSY;
    }

    port = &mesh_config->ports[mesh_config->port_count];
    memset(port, 0, sizeof(*port));
    port->enabled = true;
    mesh_uart_transport_get_default_config(&port->uart_config);
    port->uart_config.uart = uart;
    port->uart_config.io_timeout_ms = 1u;
    ++mesh_config->port_count;
    return 0;
}

int mesh_node_mini9p_init(void)
{
    struct node_vfs_config node_config;
    struct m9p_server_config server_config;
    struct mesh_node_service_config mesh_config;
    int rc;

    memset(&node_config, 0, sizeof(node_config));
    node_config.iounit = NODE_VFS_DEFAULT_IOUNIT;
    node_config.routes_text_fn = mesh_node_routes_text;
    node_config.log_text_fn = mesh_node_log_text;
    node_config.uart_text_fn = mesh_node_uart_text;
    pwos_log_init();
    rc = node_vfs_init(&g_node_vfs, &node_config);
    if (rc != 0) {
        mesh_diag_kv_u32("mesh init node_vfs rc", (uint32_t)(int32_t)rc);
        return rc;
    }

    m9p_server_get_default_config(&server_config);
    server_config.ops = node_vfs_ops();
    server_config.ops_ctx = &g_node_vfs;
    server_config.max_msize = MESH_NODE_SERVICE_FRAME_CAP;
    server_config.default_iounit = g_node_vfs.iounit;
    rc = m9p_server_init(&g_mini9p_server, &server_config);
    if (rc != 0) {
        mesh_diag_kv_u32("mesh init m9p rc", (uint32_t)(int32_t)rc);
        return rc;
    }

    memset(&mesh_config, 0, sizeof(mesh_config));
    rc = mesh_node_add_uart_port(&mesh_config, &huart1);
    if (rc != 0) {
        return rc;
    }
    rc = mesh_node_add_uart_port(&mesh_config, &huart2);
    if (rc != 0) {
        return rc;
    }
    rc = mesh_node_add_uart_port(&mesh_config, &huart3);
    if (rc != 0) {
        return rc;
    }
    rc = mesh_node_add_uart_port(&mesh_config, &huart4);
    if (rc != 0) {
        return rc;
    }
#ifdef PWOS_ENABLE_UART5_MESH
    rc = mesh_node_add_uart_port(&mesh_config, &huart5);
    if (rc != 0) {
        return rc;
    }
#endif
    rc = mesh_node_add_uart_port(&mesh_config, &huart6);
    if (rc != 0) {
        return rc;
    }
    mesh_config.mini9p_server_handler = m9p_server_handle_frame;
    mesh_config.mini9p_server_ctx = &g_mini9p_server;
    rc = mesh_node_service_init(&mesh_config);
    mesh_diag_kv_u32("mesh init service rc", (uint32_t)(int32_t)rc);
    return rc;
}
