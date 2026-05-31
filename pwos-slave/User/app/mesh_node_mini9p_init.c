#include "mesh_node_mini9p_init.h"

#include "mesh_diag.h"
#include "mesh_node_service.h"
#include "mini9p_server.h"
#include "node_vfs.h"

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

static struct node_vfs g_node_vfs;
static struct m9p_server g_mini9p_server;

int mesh_node_mini9p_init(void)
{
    struct node_vfs_config node_config;
    struct m9p_server_config server_config;
    struct mesh_node_service_config mesh_config;
    int rc;

    node_config.iounit = NODE_VFS_DEFAULT_IOUNIT;
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
    mesh_config.port_count = 2u;
    mesh_config.ports[0].enabled = true;
    mesh_uart_transport_get_default_config(&mesh_config.ports[0].uart_config);
    mesh_config.ports[0].uart_config.uart = &huart2;
    mesh_config.ports[0].uart_config.io_timeout_ms = 1u;
    mesh_config.ports[1].enabled = true;
    mesh_uart_transport_get_default_config(&mesh_config.ports[1].uart_config);
    mesh_config.ports[1].uart_config.uart = &huart1;
    mesh_config.ports[1].uart_config.io_timeout_ms = 1u;
    mesh_config.mini9p_server_handler = m9p_server_handle_frame;
    mesh_config.mini9p_server_ctx = &g_mini9p_server;
    rc = mesh_node_service_init(&mesh_config);
    mesh_diag_kv_u32("mesh init service rc", (uint32_t)(int32_t)rc);
    return rc;
}
