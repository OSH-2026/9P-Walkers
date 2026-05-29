/**
 * @file mesh_node_service.c
 * @brief STM32 slave 侧 Mesh 节点服务装配层。
 */

#include "mesh_node_service.h"

#include <string.h>

#define MESH_NODE_SERVICE_DEFAULT_LOCAL_ADDR MESH_ADDR_UNASSIGNED

struct mesh_node_service_port {
    bool initialized;
    struct mesh_uart_transport transport;
};

struct mesh_node_service_addr_port {
    bool used;
    uint8_t mesh_addr;
    uint8_t port_index;
};

struct mesh_node_service_bootstrap_pending {
    bool used;
    uint8_t uid[MESH_UID_LEN];
    uint32_t boot_nonce;
    uint8_t ingress_port;
};

struct mesh_node_service {
    bool initialized;
    size_t port_count;
    size_t next_rx_index;
    struct mesh_node_runtime runtime;
    struct mesh_node_service_port ports[MESH_NODE_SERVICE_MAX_PORTS];
    struct mesh_node_service_addr_port addr_ports[CLUSTER_MAX_NODES];
    struct mesh_node_service_bootstrap_pending pending[MESH_NODE_SERVICE_MAX_BOOTSTRAP_PENDING];
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

    for (i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        if (!service->addr_ports[i].used) {
            continue;
        }
        if (service->addr_ports[i].mesh_addr == next_hop) {
            return (int)service->addr_ports[i].port_index;
        }
    }

    return -1;
}

static int mesh_node_service_validate_port(
    const struct mesh_node_service *service,
    uint8_t port_index)
{
    if (service == NULL || port_index >= service->port_count ||
        !service->ports[port_index].initialized) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    return 0;
}

static int mesh_node_service_learn_addr_port(
    struct mesh_node_service *service,
    uint8_t mesh_addr,
    uint8_t port_index)
{
    size_t i;

    if (service == NULL || mesh_addr == MESH_ADDR_UNASSIGNED ||
        mesh_node_service_validate_port(service, port_index) != 0) {
        return 0;
    }

    for (i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        if (service->addr_ports[i].used && service->addr_ports[i].mesh_addr == mesh_addr) {
            service->addr_ports[i].port_index = port_index;
            return 0;
        }
    }

    for (i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        if (!service->addr_ports[i].used) {
            service->addr_ports[i].used = true;
            service->addr_ports[i].mesh_addr = mesh_addr;
            service->addr_ports[i].port_index = port_index;
            return 0;
        }
    }

    return -(int)MESH_ERR_BUSY;
}

static struct mesh_node_service_bootstrap_pending *mesh_node_service_find_pending_by_register(
    struct mesh_node_service *service,
    const struct mesh_register_payload *payload)
{
    size_t i;

    if (service == NULL || payload == NULL) {
        return NULL;
    }

    for (i = 0u; i < MESH_NODE_SERVICE_MAX_BOOTSTRAP_PENDING; ++i) {
        if (!service->pending[i].used) {
            continue;
        }
        if (service->pending[i].boot_nonce == payload->boot_nonce &&
            memcmp(service->pending[i].uid, payload->uid, MESH_UID_LEN) == 0) {
            return &service->pending[i];
        }
    }

    return NULL;
}

static struct mesh_node_service_bootstrap_pending *mesh_node_service_find_pending_by_uid(
    struct mesh_node_service *service,
    const uint8_t uid[MESH_UID_LEN])
{
    size_t i;

    if (service == NULL || uid == NULL) {
        return NULL;
    }

    for (i = 0u; i < MESH_NODE_SERVICE_MAX_BOOTSTRAP_PENDING; ++i) {
        if (!service->pending[i].used) {
            continue;
        }
        if (memcmp(service->pending[i].uid, uid, MESH_UID_LEN) == 0) {
            return &service->pending[i];
        }
    }

    return NULL;
}

static struct mesh_node_service_bootstrap_pending *mesh_node_service_alloc_pending(
    struct mesh_node_service *service)
{
    size_t i;

    if (service == NULL) {
        return NULL;
    }

    for (i = 0u; i < MESH_NODE_SERVICE_MAX_BOOTSTRAP_PENDING; ++i) {
        if (!service->pending[i].used) {
            return &service->pending[i];
        }
    }

    return NULL;
}

static int mesh_node_service_store_pending_register(
    struct mesh_node_service *service,
    const struct mesh_register_payload *payload,
    uint8_t ingress_port)
{
    struct mesh_node_service_bootstrap_pending *pending;

    if (payload == NULL || mesh_node_service_validate_port(service, ingress_port) != 0) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    pending = mesh_node_service_find_pending_by_register(service, payload);
    if (pending == NULL) {
        pending = mesh_node_service_alloc_pending(service);
        if (pending == NULL) {
            return -(int)MESH_ERR_BUSY;
        }
    }

    pending->used = true;
    memcpy(pending->uid, payload->uid, MESH_UID_LEN);
    pending->boot_nonce = payload->boot_nonce;
    pending->ingress_port = ingress_port;
    return 0;
}

static int mesh_node_service_send_to_port(
    struct mesh_node_service *service,
    uint8_t port_index,
    const uint8_t *tx_data,
    size_t tx_len)
{
    int rc;

    rc = mesh_node_service_validate_port(service, port_index);
    if (rc != 0) {
        return rc;
    }

    return mesh_uart_transport_send_frame(&service->ports[port_index].transport, tx_data, tx_len);
}

static int mesh_node_service_find_port_for_runtime_next_hop(
    const struct mesh_node_service *service,
    uint8_t next_hop)
{
    return mesh_node_service_find_port_for_next_hop(service, next_hop);
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

static int mesh_node_service_forward_bootstrap_register(
    struct mesh_node_service *service,
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len,
    const struct mesh_register_payload *payload,
    uint8_t ingress_port)
{
    uint8_t next_hop = 0u;
    bool is_local = false;
    int port_index;
    int rc;

    rc = mesh_node_service_store_pending_register(service, payload, ingress_port);
    if (rc != 0) {
        return rc;
    }

    rc = cluster_lookup_next_hop(&runtime->cluster, MESH_ADDR_HOST, &next_hop, &is_local);
    if (rc != 0 || is_local) {
        return rc != 0 ? rc : -(int)MESH_ERR_NO_ROUTE;
    }

    port_index = mesh_node_service_find_port_for_runtime_next_hop(service, next_hop);
    if (port_index < 0) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    return mesh_node_service_send_to_port(service, (uint8_t)port_index, frame_data, frame_len);
}

static int mesh_node_service_forward_bootstrap_assign(
    struct mesh_node_service *service,
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len,
    const struct mesh_assign_payload *payload,
    bool *out_handled)
{
    struct mesh_node_service_bootstrap_pending *pending;
    uint8_t ingress_port;
    int rc;

    pending = mesh_node_service_find_pending_by_uid(service, payload->uid);
    if (pending == NULL) {
        *out_handled = false;
        return 0;
    }

    ingress_port = pending->ingress_port;
    rc = mesh_node_service_send_to_port(service, ingress_port, frame_data, frame_len);
    if (rc != 0) {
        return rc;
    }

    rc = mesh_node_service_learn_addr_port(service, payload->node_addr, ingress_port);
    if (rc != 0) {
        return rc;
    }
    rc = cluster_add_route(&runtime->cluster, payload->node_addr, ingress_port, 1u);
    if (rc != 0) {
        return rc;
    }
    rc = mesh_node_runtime_report_neighbor_link(runtime, payload->node_addr, ingress_port, true);
    if (rc != 0) {
        return rc;
    }

    memset(pending, 0, sizeof(*pending));
    *out_handled = true;
    return 0;
}

static int mesh_node_service_bootstrap_bridge(
    void *bridge_ctx,
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len,
    const struct mesh_frame_view *frame,
    uint8_t ingress_port,
    bool *out_handled)
{
    struct mesh_node_service *service = (struct mesh_node_service *)bridge_ctx;

    if (out_handled == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    *out_handled = false;

    if (service == NULL || runtime == NULL || frame_data == NULL || frame == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (ingress_port == MESH_PROCESSER_INGRESS_PORT_NONE) {
        return 0;
    }

    if (frame->type == MESH_TYPE_REGISTER &&
        frame->src == MESH_ADDR_UNASSIGNED &&
        frame->dst == MESH_ADDR_UNASSIGNED) {
        struct mesh_register_payload payload;
        int rc;

        if (!mesh_parse_register(frame, &payload)) {
            return -(int)MESH_ERR_BAD_FRAME;
        }
        rc = mesh_node_service_forward_bootstrap_register(
            service,
            runtime,
            frame_data,
            frame_len,
            &payload,
            ingress_port);
        if (rc != 0) {
            return rc;
        }
        *out_handled = true;
        return 0;
    }

    if (frame->type == MESH_TYPE_ASSIGN) {
        struct mesh_assign_payload payload;

        if (!mesh_parse_assign(frame, &payload)) {
            return -(int)MESH_ERR_BAD_FRAME;
        }
        return mesh_node_service_forward_bootstrap_assign(
            service,
            runtime,
            frame_data,
            frame_len,
            &payload,
            out_handled);
    }

    return 0;
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

        rc = mesh_uart_transport_receive_frame(&service->ports[index].transport, rx_data, rx_cap, rx_len);
        if (rc == 0) {
            struct mesh_frame_view frame;

            *out_ingress_port = (uint8_t)index;
            if (mesh_decode_frame(rx_data, *rx_len, &frame)) {
                (void)mesh_node_service_learn_addr_port(service, frame.src, (uint8_t)index);
            }
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
    runtime_config.receive_frame = mesh_node_service_receive_frame;
    runtime_config.transport_ctx = &g_mesh_node_service;
    runtime_config.bootstrap_bridge = mesh_node_service_bootstrap_bridge;
    runtime_config.bootstrap_bridge_ctx = &g_mesh_node_service;
    runtime_config.mini9p_server_handler = config->mini9p_server_handler;
    runtime_config.mini9p_server_ctx = config->mini9p_server_ctx;
    mesh_node_service_fill_local_uid(runtime_config.local_uid);
    runtime_config.boot_nonce = mesh_node_service_make_boot_nonce();
    runtime_config.capability_bits = MESH_NODE_SERVICE_REGISTER_CAPABILITY_BITS;
    runtime_config.port_bitmap = mesh_node_service_make_port_bitmap(&g_mesh_node_service);
    runtime_config.local_addr = MESH_NODE_SERVICE_DEFAULT_LOCAL_ADDR;
    g_mesh_node_service.initialized = true;
    rc = mesh_node_runtime_init(&g_mesh_node_service.runtime, &runtime_config, enabled_count);
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
