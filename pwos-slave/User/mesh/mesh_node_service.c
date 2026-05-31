/**
 * @file mesh_node_service.c
 * @brief STM32 slave 侧多 UART mesh service 实现。
 */

#include "mesh_node_service.h"

#include "../app/mesh_diag.h"

#include <stdio.h>
#include <string.h>

#define MESH_NODE_SERVICE_DEFAULT_LOCAL_ADDR MESH_ADDR_UNASSIGNED

struct mesh_node_service_addr_port {
    bool used;
    uint8_t mesh_addr;
    uint8_t port_id;
};

struct mesh_node_service_port {
    bool initialized;
    struct mesh_uart_transport transport;
};

struct mesh_node_service {
    bool initialized;
    size_t port_count;
    size_t next_rx_index;
    struct mesh_node_runtime runtime;
    struct mesh_node_service_port ports[MESH_NODE_SERVICE_MAX_PORTS];
    struct mesh_node_service_addr_port addr_ports[MESH_NODE_SERVICE_MAX_PORTS];
};

static struct mesh_node_service g_mesh_node_service;
static bool g_mesh_node_service_initialized;

static bool mesh_node_service_error_is_soft(int rc)
{
    return rc == -(int)MESH_ERR_BUSY || rc == -(int)MESH_ERR_NO_ROUTE;
}

static bool mesh_node_service_port_is_ready(const struct mesh_node_service *service, uint8_t port_id)
{
    return service != NULL &&
        port_id < service->port_count &&
        service->ports[port_id].initialized;
}

static int mesh_node_service_find_port_for_next_hop(
    const struct mesh_node_service *service,
    uint8_t next_hop)
{
    size_t i;

    if (service == NULL) {
        return -1;
    }

    for (i = 0u; i < MESH_NODE_SERVICE_MAX_PORTS; ++i) {
        if (!service->addr_ports[i].used) {
            continue;
        }
        if (service->addr_ports[i].mesh_addr == next_hop &&
            mesh_node_service_port_is_ready(service, service->addr_ports[i].port_id)) {
            return (int)service->addr_ports[i].port_id;
        }
    }

    return -1;
}

static int mesh_node_service_learn_addr_port_ctx(void *ctx, uint8_t mesh_addr, uint8_t port_id)
{
    struct mesh_node_service *service = (struct mesh_node_service *)ctx;
    struct mesh_node_service_addr_port *free_slot = NULL;
    size_t i;

    if (service == NULL || !service->initialized || mesh_addr == MESH_ADDR_UNASSIGNED ||
        !mesh_node_service_port_is_ready(service, port_id)) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    for (i = 0u; i < MESH_NODE_SERVICE_MAX_PORTS; ++i) {
        if (!service->addr_ports[i].used) {
            if (free_slot == NULL) {
                free_slot = &service->addr_ports[i];
            }
            continue;
        }
        if (service->addr_ports[i].mesh_addr == mesh_addr) {
            service->addr_ports[i].port_id = port_id;
            return 0;
        }
    }

    if (free_slot == NULL) {
        return -(int)MESH_ERR_BUSY;
    }

    free_slot->used = true;
    free_slot->mesh_addr = mesh_addr;
    free_slot->port_id = port_id;
    return 0;
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

static uint32_t mesh_node_service_time_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
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
                mesh_diag_send_frame((uint8_t)i, next_hop, tx_len, rc);
                first_error = rc;
            }
        }
        return first_error;
    }

    port_index = mesh_node_service_find_port_for_next_hop(service, next_hop);
    if (port_index < 0) {
        mesh_diag_send_frame(MESH_PROCESSER_INGRESS_PORT_NONE, next_hop, tx_len, -(int)MESH_ERR_NO_ROUTE);
        return -(int)MESH_ERR_NO_ROUTE;
    }

    {
        int rc = mesh_uart_transport_send_frame(&service->ports[port_index].transport, tx_data, tx_len);

        if (rc != 0) {
            mesh_diag_send_frame((uint8_t)port_index, next_hop, tx_len, rc);
        }
        return rc;
    }
}

/*
 * 向指定物理端口直接发送帧，不走 addr->port 查表。
 * 用于 NEIGHBOR_PROBE_RESPONSE 等 port-local 回复。
 */
static int mesh_node_service_send_frame_to_port(
    void *ctx,
    uint8_t port_id,
    const uint8_t *tx_data,
    size_t tx_len)
{
    struct mesh_node_service *service = (struct mesh_node_service *)ctx;

    if (service == NULL || !service->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (port_id >= service->port_count || !service->ports[port_id].initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    {
        int rc = mesh_uart_transport_send_frame(&service->ports[port_id].transport, tx_data, tx_len);

        if (rc != 0) {
            mesh_diag_send_frame(port_id, port_id, tx_len, rc);
        }
        return rc;
    }
}

static int mesh_node_service_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len,
    uint8_t *out_ingress_port)
{
    struct mesh_node_service *service = (struct mesh_node_service *)transport_ctx;
    int soft_error = -(int)MESH_ERR_BUSY;
    size_t checked;

    if (service == NULL || !service->initialized || rx_data == NULL || rx_len == NULL ||
        out_ingress_port == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    *rx_len = 0u;
    *out_ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;
    for (checked = 0u; checked < service->port_count; ++checked) {
        size_t index = (service->next_rx_index + checked) % service->port_count;
        int rc;

        if (!service->ports[index].initialized) {
            continue;
        }
        if (!mesh_uart_transport_rx_pending(&service->ports[index].transport)) {
            continue;
        }

        rc = mesh_uart_transport_receive_frame(&service->ports[index].transport, rx_data, rx_cap, rx_len);
        if (rc == 0) {
            *out_ingress_port = (uint8_t)index;
            service->next_rx_index = (index + 1u) % service->port_count;
            return 0;
        }
        if (mesh_node_service_error_is_soft(rc)) {
            soft_error = rc;
            continue;
        }
        if (rc == -(int)MESH_ERR_BAD_FRAME) {
            (void)mesh_uart_transport_flush_input(&service->ports[index].transport);
            mesh_diag_recv_frame((uint8_t)index, 0u, rc);
            soft_error = -(int)MESH_ERR_BUSY;
            continue;
        }

        mesh_diag_recv_frame((uint8_t)index, 0u, rc);
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

        rc = mesh_uart_transport_init(
            &g_mesh_node_service.ports[i].transport,
            &config->ports[i].uart_config);
        if (rc != 0) {
            mesh_node_service_deinit();
            return rc;
        }

        g_mesh_node_service.ports[i].initialized = true;
        ++enabled_count;
    }

    if (enabled_count == 0u) {
        memset(&g_mesh_node_service, 0, sizeof(g_mesh_node_service));
        return -(int)MESH_ERR_INVALID_STATE;
    }

    mesh_node_runtime_get_default_config(&runtime_config);
    runtime_config.send_frame = mesh_node_service_send_frame;
    runtime_config.send_frame_to_port = mesh_node_service_send_frame_to_port;
    runtime_config.receive_frame = mesh_node_service_receive_frame;
    runtime_config.transport_ctx = &g_mesh_node_service;
    runtime_config.learn_peer_port = mesh_node_service_learn_addr_port_ctx;
    runtime_config.learn_peer_port_ctx = &g_mesh_node_service;
    runtime_config.time_ms = mesh_node_service_time_ms;
    runtime_config.time_ctx = &g_mesh_node_service;
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

int mesh_node_service_learn_addr_port(uint8_t mesh_addr, uint8_t port_id)
{
    if (!g_mesh_node_service_initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_service_learn_addr_port_ctx(&g_mesh_node_service, mesh_addr, port_id);
}

int mesh_node_service_format_addr_ports(char *out, size_t out_cap)
{
    size_t used = 0u;
    size_t i;

    if (!g_mesh_node_service_initialized || out == NULL || out_cap == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    for (i = 0u; i < MESH_NODE_SERVICE_MAX_PORTS && used < out_cap - 1u; ++i) {
        const struct mesh_node_service_addr_port *entry = &g_mesh_node_service.addr_ports[i];
        int written;

        if (!entry->used) {
            continue;
        }

        written = snprintf(
            out + used,
            out_cap - used,
            "addr_port addr=0x%02x port=%u\n",
            entry->mesh_addr,
            (unsigned)entry->port_id);
        if (written < 0) {
            return -(int)MESH_ERR_BAD_FRAME;
        }
        if ((size_t)written >= out_cap - used) {
            used = out_cap - 1u;
            break;
        }
        used += (size_t)written;
    }

    out[used] = '\0';
    return 0;
}
