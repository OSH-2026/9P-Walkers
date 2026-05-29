#include "mesh_host_service.h"

#include <string.h>

static struct mesh_host_service g_default_manager;

static bool mesh_host_service_error_is_soft(int rc)
{
    return rc == -(int)MESH_ERR_BUSY || rc == -(int)MESH_ERR_NO_ROUTE;
}

static size_t mesh_host_service_count_ports(const struct mesh_host_service *manager)
{
    size_t count = 0u;
    size_t i;

    for (i = 0u; i < manager->port_count; ++i) {
        if (manager->ports[i].initialized) {
            ++count;
        }
    }

    return count;
}

static int mesh_host_service_find_single_port(const struct mesh_host_service *manager)
{
    size_t i;

    for (i = 0u; i < manager->port_count; ++i) {
        if (manager->ports[i].initialized) {
            return (int)i;
        }
    }

    return -1;
}

static int mesh_host_service_find_port_for_next_hop(
    const struct mesh_host_service *manager,
    uint8_t next_hop)
{
    size_t i;

    if (mesh_host_service_count_ports(manager) == 1u) {
        return mesh_host_service_find_single_port(manager);
    }

    for (i = 0u; i < manager->port_count; ++i) {
        if (!manager->ports[i].initialized) {
            continue;
        }
        if (manager->ports[i].neighbor_addr == next_hop) {
            return (int)i;
        }
    }

    return -1;
}

static bool mesh_host_service_has_duplicate_neighbor(
    const struct mesh_host_service_config *config,
    size_t port_index)
{
    size_t i;
    uint8_t neighbor = config->ports[port_index].neighbor_addr;

    if (neighbor == MESH_HOST_SERVICE_NEIGHBOR_ANY) {
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

void mesh_host_service_get_default_config(struct mesh_host_service_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->port_count = 1u;
    out_config->ports[0].enabled = true;
    out_config->ports[0].neighbor_addr = MESH_HOST_SERVICE_NEIGHBOR_ANY;
    mesh_uart_transport_get_default_config(&out_config->ports[0].uart_config);
}

int mesh_host_service_init(
    struct mesh_host_service *manager,
    const struct mesh_host_service_config *config)
{
    size_t i;
    size_t enabled_count = 0u;

    if (manager == NULL || config == NULL || config->port_count == 0u ||
        config->port_count > MESH_HOST_SERVICE_MAX_PORTS) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    memset(manager, 0, sizeof(*manager));
    manager->port_count = config->port_count;

    for (i = 0u; i < config->port_count; ++i) {
        int rc;

        if (!config->ports[i].enabled) {
            continue;
        }
        if (mesh_host_service_has_duplicate_neighbor(config, i)) {
            mesh_host_service_deinit(manager);
            return -(int)MESH_ERR_INVALID_STATE;
        }

        rc = mesh_uart_transport_init(&manager->ports[i].transport, &config->ports[i].uart_config);
        if (rc != 0) {
            mesh_host_service_deinit(manager);
            return rc;
        }

        manager->ports[i].neighbor_addr = config->ports[i].neighbor_addr;
        manager->ports[i].initialized = true;
        ++enabled_count;
    }

    if (enabled_count == 0u) {
        memset(manager, 0, sizeof(*manager));
        return -(int)MESH_ERR_INVALID_STATE;
    }

    manager->next_rx_index = 0u;
    manager->initialized = true;
    return 0;
}

void mesh_host_service_deinit(struct mesh_host_service *manager)
{
    size_t i;

    if (manager == NULL) {
        return;
    }

    for (i = 0u; i < manager->port_count; ++i) {
        if (manager->ports[i].initialized) {
            mesh_uart_transport_deinit(&manager->ports[i].transport);
        }
    }

    memset(manager, 0, sizeof(*manager));
}

int mesh_host_service_init_default(void)
{
    struct mesh_host_service_config config;

    if (g_default_manager.initialized) {
        return 0;
    }

    mesh_host_service_get_default_config(&config);
    return mesh_host_service_init(&g_default_manager, &config);
}

void mesh_host_service_deinit_default(void)
{
    mesh_host_service_deinit(&g_default_manager);
}

struct mesh_host_service *mesh_host_service_default(void)
{
    if (!g_default_manager.initialized) {
        return NULL;
    }

    return &g_default_manager;
}

int mesh_host_service_send_frame(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len)
{
    struct mesh_host_service *manager = (struct mesh_host_service *)transport_ctx;
    int port_index;

    if (manager == NULL || !manager->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    port_index = mesh_host_service_find_port_for_next_hop(manager, next_hop);
    if (port_index < 0) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    return mesh_uart_transport_send_frame(&manager->ports[port_index].transport, tx_data, tx_len);
}

int mesh_host_service_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    struct mesh_host_service *manager = (struct mesh_host_service *)transport_ctx;
    int soft_error = -(int)MESH_ERR_BUSY;
    size_t checked;

    if (manager == NULL || !manager->initialized || rx_data == NULL || rx_len == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    *rx_len = 0u;
    for (checked = 0u; checked < manager->port_count; ++checked) {
        size_t index = (manager->next_rx_index + checked) % manager->port_count;
        int rc;

        if (!manager->ports[index].initialized) {
            continue;
        }

        rc = mesh_uart_transport_receive_frame(&manager->ports[index].transport, rx_data, rx_cap, rx_len);
        if (rc == 0) {
            manager->next_rx_index = (index + 1u) % manager->port_count;
            return 0;
        }
        if (mesh_host_service_error_is_soft(rc)) {
            soft_error = rc;
            continue;
        }

        return rc;
    }

    return soft_error;
}
