/**
 * @file mesh_node_service.c
 * @brief STM32 slave 侧 Mesh 节点服务装配层。
 */

#include "mesh_node_service.h"

#include <string.h>

#define MESH_NODE_SERVICE_DEFAULT_LOCAL_ADDR MESH_ADDR_UNASSIGNED

struct mesh_node_service_port {
    bool initialized;
    uint8_t neighbor_addr;
    struct mesh_uart_transport transport;
};

struct mesh_node_service {
    bool initialized;
    size_t port_count;
    size_t next_rx_index;
    struct mesh_node_runtime runtime;
    struct mesh_node_service_port ports[MESH_NODE_SERVICE_MAX_PORTS];
};

static struct mesh_node_service g_mesh_node_service;
static bool g_mesh_node_service_initialized;

static bool mesh_node_service_error_is_soft(int rc)
{
    return rc == -(int)MESH_ERR_BUSY || rc == -(int)MESH_ERR_NO_ROUTE;
}

static size_t mesh_node_service_count_ports(const struct mesh_node_service *service)
{
    size_t count = 0u;
    size_t i;

    for (i = 0u; i < service->port_count; ++i) {
        if (service->ports[i].initialized) {
            ++count;
        }
    }

    return count;
}

static int mesh_node_service_find_single_port(const struct mesh_node_service *service)
{
    size_t i;

    for (i = 0u; i < service->port_count; ++i) {
        if (service->ports[i].initialized) {
            return (int)i;
        }
    }

    return -1;
}

static int mesh_node_service_find_port_for_next_hop(
    const struct mesh_node_service *service,
    uint8_t next_hop)
{
    size_t i;

    if (mesh_node_service_count_ports(service) == 1u) {
        return mesh_node_service_find_single_port(service);
    }

    for (i = 0u; i < service->port_count; ++i) {
        if (!service->ports[i].initialized) {
            continue;
        }
        if (service->ports[i].neighbor_addr == next_hop) {
            return (int)i;
        }
    }

    return -1;
}

static bool mesh_node_service_has_duplicate_neighbor(
    const struct mesh_node_service_config *config,
    size_t port_index)
{
    size_t i;
    uint8_t neighbor = config->ports[port_index].neighbor_addr;

    if (neighbor == MESH_NODE_SERVICE_NEIGHBOR_ANY) {
        return false;
    }

    for (i = 0u; i < port_index; ++i) {
        if (!config->ports[i].enabled) {
            continue;
        }
        if (config->ports[i].neighbor_addr == neighbor) {
            return true;
        }
    }

    return false;
}

static uint8_t mesh_node_service_make_port_bitmap(const struct mesh_node_service *service)
{
    uint8_t bitmap = 0u;
    size_t i;

    for (i = 0u; i < service->port_count && i < 8u; ++i) {
        if (service->ports[i].initialized) {
            bitmap = (uint8_t)(bitmap | (uint8_t)(1u << i));
        }
    }

    return bitmap;
}

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
    struct mesh_node_service *service = (struct mesh_node_service *)transport_ctx;
    int port_index;
    size_t i;
    int first_error = 0;

    if (service == NULL || !service->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    if (next_hop == MESH_NODE_SERVICE_NEIGHBOR_ANY) {
        for (i = 0u; i < service->port_count; ++i) {
            int rc;

            if (!service->ports[i].initialized) {
                continue;
            }
            rc = mesh_uart_transport_send_frame(&service->ports[i].transport, tx_data, tx_len);
            if (rc != 0 && first_error == 0) {
                first_error = rc;
            }
        }
        return first_error;
    }

    port_index = mesh_node_service_find_port_for_next_hop(service, next_hop);
    if (port_index < 0) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    return mesh_uart_transport_send_frame(&service->ports[port_index].transport, tx_data, tx_len);
}

static int mesh_node_service_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    struct mesh_node_service *service = (struct mesh_node_service *)transport_ctx;
    int soft_error = -(int)MESH_ERR_BUSY;
    size_t checked;

    if (service == NULL || !service->initialized || rx_data == NULL || rx_len == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    *rx_len = 0u;
    for (checked = 0u; checked < service->port_count; ++checked) {
        size_t index = (service->next_rx_index + checked) % service->port_count;
        int rc;

        if (!service->ports[index].initialized) {
            continue;
        }

        rc = mesh_uart_transport_receive_frame(&service->ports[index].transport, rx_data, rx_cap, rx_len);
        if (rc == 0) {
            service->next_rx_index = (index + 1u) % service->port_count;
            return 0;
        }
        if (mesh_node_service_error_is_soft(rc)) {
            soft_error = rc;
            continue;
        }

        return rc;
    }

    return soft_error;
}

void mesh_node_service_get_default_config(struct mesh_node_service_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->port_count = 1u;
    out_config->ports[0].enabled = true;
    out_config->ports[0].neighbor_addr = MESH_NODE_SERVICE_NEIGHBOR_ANY;
    mesh_uart_transport_get_default_config(&out_config->ports[0].uart_config);
}

int mesh_node_service_init(const struct mesh_node_service_config *config)
{
    struct mesh_node_runtime_config runtime_config;
    size_t i;
    size_t enabled_count = 0u;
    int rc;

    if (config == NULL || config->mini9p_server_handler == NULL || config->port_count == 0u ||
        config->port_count > MESH_NODE_SERVICE_MAX_PORTS) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    if (g_mesh_node_service_initialized) {
        mesh_node_service_deinit();
    }

    memset(&g_mesh_node_service, 0, sizeof(g_mesh_node_service));
    g_mesh_node_service.port_count = config->port_count;

    for (i = 0u; i < config->port_count; ++i) {
        if (!config->ports[i].enabled) {
            continue;
        }
        if (mesh_node_service_has_duplicate_neighbor(config, i)) {
            mesh_node_service_deinit();
            return -(int)MESH_ERR_INVALID_STATE;
        }

        rc = mesh_uart_transport_init(
            &g_mesh_node_service.ports[i].transport,
            &config->ports[i].uart_config);
        if (rc != 0) {
            mesh_node_service_deinit();
            return rc;
        }

        g_mesh_node_service.ports[i].neighbor_addr = config->ports[i].neighbor_addr;
        g_mesh_node_service.ports[i].initialized = true;
        ++enabled_count;
    }

    if (enabled_count == 0u) {
        memset(&g_mesh_node_service, 0, sizeof(g_mesh_node_service));
        return -(int)MESH_ERR_INVALID_STATE;
    }

    mesh_node_runtime_get_default_config(&runtime_config);
    runtime_config.send_frame = mesh_node_service_send_frame;
    runtime_config.receive_frame = mesh_node_service_receive_frame;
    runtime_config.transport_ctx = &g_mesh_node_service;
    runtime_config.mini9p_server_handler = config->mini9p_server_handler;
    runtime_config.mini9p_server_ctx = config->mini9p_server_ctx;
    mesh_node_service_fill_local_uid(runtime_config.local_uid);
    runtime_config.boot_nonce = mesh_node_service_make_boot_nonce();
    runtime_config.capability_bits = MESH_NODE_SERVICE_REGISTER_CAPABILITY_BITS;
    runtime_config.port_bitmap = mesh_node_service_make_port_bitmap(&g_mesh_node_service);
    runtime_config.bootstrap_next_hop = MESH_NODE_SERVICE_NEIGHBOR_ANY;
    runtime_config.local_addr = MESH_NODE_SERVICE_DEFAULT_LOCAL_ADDR;
    g_mesh_node_service.initialized = true;
    rc = mesh_node_runtime_init(&g_mesh_node_service.runtime, &runtime_config);
    if (rc != 0) {
        mesh_node_service_deinit();
        return rc;
    }

    g_mesh_node_service_initialized = true;
    return 0;
}

void mesh_node_service_deinit(void)
{
    size_t i;

    mesh_node_runtime_deinit(&g_mesh_node_service.runtime);

    for (i = 0u; i < g_mesh_node_service.port_count; ++i) {
        if (g_mesh_node_service.ports[i].initialized) {
            mesh_uart_transport_deinit(&g_mesh_node_service.ports[i].transport);
        }
    }

    memset(&g_mesh_node_service, 0, sizeof(g_mesh_node_service));
    g_mesh_node_service_initialized = false;
}

int mesh_node_service_notify_link_up(void)
{
    if (!g_mesh_node_service_initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_runtime_notify_link_up(&g_mesh_node_service.runtime);
}

int mesh_node_service_poll_once(void)
{
    if (!g_mesh_node_service_initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_runtime_poll_once(&g_mesh_node_service.runtime);
}

struct mesh_node_runtime *mesh_node_service_runtime(void)
{
    if (!g_mesh_node_service_initialized) {
        return NULL;
    }

    return &g_mesh_node_service.runtime;
}
