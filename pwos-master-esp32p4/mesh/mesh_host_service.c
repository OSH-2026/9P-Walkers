#include "mesh_host_service.h"

#include <string.h>

#define PWOS_MASTER_MESH_UART_BAUD_RATE 1000000
#define PWOS_MASTER_MESH_UART_PORT 1
#define PWOS_MASTER_MESH_UART_TX_PIN 37
#define PWOS_MASTER_MESH_UART_RX_PIN 38

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mesh_wifi_link.h"
#endif

static struct mesh_host_service g_default_service;

#ifdef ESP_PLATFORM
static TaskHandle_t g_default_service_task;
#endif

static bool mesh_host_service_error_is_soft(int rc)
{
    return rc == -(int)MESH_ERR_BUSY || rc == -(int)MESH_ERR_NO_ROUTE;
}

#ifdef ESP_PLATFORM
static void mesh_host_service_default_task(void *arg)
{
    struct mesh_host_service *service = (struct mesh_host_service *)arg;

    for (;;) {
        int rc = mesh_host_runtime_poll_once(&service->runtime);

        if (rc != 0) {
            vTaskDelay(pdMS_TO_TICKS(MESH_HOST_RUNTIME_IDLE_DELAY_MS));
        }
    }
}
#endif

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
#ifdef ESP_PLATFORM
    out_config->ports[0].uart_config.uart_port = PWOS_MASTER_MESH_UART_PORT;
    out_config->ports[0].uart_config.tx_pin = PWOS_MASTER_MESH_UART_TX_PIN;
    out_config->ports[0].uart_config.rx_pin = PWOS_MASTER_MESH_UART_RX_PIN;
    out_config->ports[0].uart_config.baud_rate = PWOS_MASTER_MESH_UART_BAUD_RATE;
#endif
}

int mesh_host_service_init(
    struct mesh_host_service *manager,
    const struct mesh_host_service_config *config)
{
    struct mesh_host_runtime_config runtime_config;
    size_t i;
    size_t enabled_count = 0u;
    int rc;

    if (manager == NULL || config == NULL || config->port_count == 0u ||
        config->port_count > MESH_HOST_SERVICE_MAX_PORTS) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    memset(manager, 0, sizeof(*manager));
    manager->port_count = config->port_count;

    for (i = 0u; i < config->port_count; ++i) {
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

    rc = cluster_config_init_mesh_host();
    if (rc != 0) {
        mesh_host_service_deinit(manager);
        return rc;
    }

    mesh_host_runtime_get_default_config(&runtime_config);
    runtime_config.mesh_cluster = cluster_config_mesh_cluster();
    runtime_config.transport_ctx = manager;
    runtime_config.send_frame = mesh_host_service_send_frame;
    runtime_config.receive_frame = mesh_host_service_receive_frame;
    rc = mesh_host_runtime_init(&manager->runtime, &runtime_config);
    if (rc != 0) {
        mesh_host_service_deinit(manager);
        return rc;
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

    mesh_host_runtime_deinit(&manager->runtime);

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

    if (g_default_service.initialized) {
        return 0;
    }

    mesh_host_service_get_default_config(&config);
    return mesh_host_service_init(&g_default_service, &config);
}

void mesh_host_service_deinit_default(void)
{
    mesh_host_service_deinit(&g_default_service);
}

struct mesh_host_service *mesh_host_service_default(void)
{
    if (!g_default_service.initialized) {
        return NULL;
    }

    return &g_default_service;
}

struct mesh_host_runtime *mesh_host_service_default_runtime(void)
{
    if (!g_default_service.initialized || !g_default_service.runtime.initialized) {
        return NULL;
    }

    return &g_default_service.runtime;
}

int mesh_host_service_start_default_task(void)
{
    int rc;

    rc = mesh_host_service_init_default();
    if (rc != 0) {
        return rc;
    }

#ifdef ESP_PLATFORM
    if (g_default_service_task != NULL) {
        return 0;
    }
    if (xTaskCreate(
            mesh_host_service_default_task,
            "mesh_host_rt",
            MESH_HOST_RUNTIME_TASK_STACK_SIZE,
            &g_default_service,
            MESH_HOST_RUNTIME_TASK_PRIORITY,
            &g_default_service_task) != pdPASS) {
        return -(int)M9P_ERR_EIO;
    }
    return 0;
#else
    return -(int)M9P_ERR_ENOTSUP;
#endif
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

#ifdef ESP_PLATFORM
    if (mesh_wifi_link_active()) {
        /* 已学到的地址经 WiFi/TCP 链路单播。 */
        if (mesh_wifi_link_owns_addr(next_hop)) {
            return mesh_wifi_link_send_frame(tx_data, tx_len);
        }
        /* bootstrap 帧（如 ASSIGN，dst=0xFF）无法路由，向两类链路广播，
         * 保证 WiFi 侧未分配节点也能收到地址分配。 */
        if (next_hop == MESH_ADDR_UNASSIGNED) {
            int wifi_rc = mesh_wifi_link_send_frame(tx_data, tx_len);
            int uart_rc = -(int)MESH_ERR_NO_ROUTE;

            port_index = mesh_host_service_find_port_for_next_hop(manager, next_hop);
            if (port_index >= 0) {
                uart_rc = mesh_uart_transport_send_frame(
                    &manager->ports[port_index].transport, tx_data, tx_len);
            }
            return (uart_rc == 0 || wifi_rc == 0) ? 0 : uart_rc;
        }
    }
#endif

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
    size_t *rx_len,
    uint8_t *out_ingress_port)
{
    struct mesh_host_service *manager = (struct mesh_host_service *)transport_ctx;
    int soft_error = -(int)MESH_ERR_BUSY;
    size_t checked;

    if (manager == NULL || !manager->initialized || rx_data == NULL || rx_len == NULL ||
        out_ingress_port == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    *rx_len = 0u;
    *out_ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;

#ifdef ESP_PLATFORM
    /* WiFi/TCP 链路为非阻塞轮询，先于 UART 扫描以降低帧延迟。 */
    if (mesh_wifi_link_active()) {
        int wifi_rc = mesh_wifi_link_receive_frame(rx_data, rx_cap, rx_len);

        if (wifi_rc == 0) {
            *out_ingress_port = MESH_HOST_SERVICE_WIFI_INGRESS_PORT;
            return 0;
        }
        /* WiFi 侧错误一律视为软错误，继续扫描 UART 端口。 */
    }
#endif

    for (checked = 0u; checked < manager->port_count; ++checked) {
        size_t index = (manager->next_rx_index + checked) % manager->port_count;
        int rc;

        if (!manager->ports[index].initialized) {
            continue;
        }

        rc = mesh_uart_transport_receive_frame(&manager->ports[index].transport, rx_data, rx_cap, rx_len);
        if (rc == 0) {
            *out_ingress_port = (uint8_t)index;
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
