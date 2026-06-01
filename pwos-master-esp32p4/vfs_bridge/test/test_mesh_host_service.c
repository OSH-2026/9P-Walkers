#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mesh_host_service.h"

#define FAKE_PORT_COUNT 4u
#define FAKE_RX_CAP 8u
#define FAKE_INIT_FAIL ((void *)0xffu)

struct fake_port_state {
    bool initialized;
    int receive_rc;
    uint8_t rx_data[FAKE_RX_CAP];
    size_t rx_len;
    uint8_t last_tx[FAKE_RX_CAP];
    size_t last_tx_len;
    size_t send_count;
    size_t receive_count;
};

static struct fake_port_state g_fake_ports[FAKE_PORT_COUNT];

static void fake_ports_reset(void)
{
    memset(g_fake_ports, 0, sizeof(g_fake_ports));
    for (size_t i = 0u; i < FAKE_PORT_COUNT; ++i) {
        g_fake_ports[i].receive_rc = -(int)MESH_ERR_BUSY;
    }
}

static void fake_queue_rx(size_t port, const uint8_t *data, size_t len)
{
    assert(port < FAKE_PORT_COUNT);
    assert(len <= FAKE_RX_CAP);
    memcpy(g_fake_ports[port].rx_data, data, len);
    g_fake_ports[port].rx_len = len;
    g_fake_ports[port].receive_rc = 0;
}

void mesh_uart_transport_get_default_config(struct mesh_uart_transport_config *out_config)
{
    if (out_config != NULL) {
        memset(out_config, 0, sizeof(*out_config));
        out_config->io_timeout_ms = MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    }
}

int mesh_uart_transport_init(
    struct mesh_uart_transport *transport,
    const struct mesh_uart_transport_config *config)
{
    uintptr_t port;

    if (transport == NULL || config == NULL || config->uart == FAKE_INIT_FAIL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    port = (uintptr_t)config->uart;
    if (port >= FAKE_PORT_COUNT) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    transport->config = *config;
    transport->initialized = true;
    g_fake_ports[port].initialized = true;
    return 0;
}

void mesh_uart_transport_deinit(struct mesh_uart_transport *transport)
{
    uintptr_t port;

    if (transport == NULL || !transport->initialized) {
        return;
    }

    port = (uintptr_t)transport->config.uart;
    if (port < FAKE_PORT_COUNT) {
        g_fake_ports[port].initialized = false;
    }
    memset(transport, 0, sizeof(*transport));
}

int mesh_uart_transport_send_frame(
    struct mesh_uart_transport *transport,
    const uint8_t *tx_data,
    size_t tx_len)
{
    uintptr_t port;

    if (transport == NULL || !transport->initialized || tx_data == NULL || tx_len > FAKE_RX_CAP) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    port = (uintptr_t)transport->config.uart;
    if (port >= FAKE_PORT_COUNT) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    memcpy(g_fake_ports[port].last_tx, tx_data, tx_len);
    g_fake_ports[port].last_tx_len = tx_len;
    ++g_fake_ports[port].send_count;
    return 0;
}

int mesh_uart_transport_receive_frame(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    uintptr_t port;
    struct fake_port_state *state;

    if (transport == NULL || !transport->initialized || rx_data == NULL || rx_len == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    port = (uintptr_t)transport->config.uart;
    if (port >= FAKE_PORT_COUNT) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    state = &g_fake_ports[port];
    ++state->receive_count;
    if (state->receive_rc != 0) {
        *rx_len = 0u;
        return state->receive_rc;
    }
    if (state->rx_len > rx_cap) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    memcpy(rx_data, state->rx_data, state->rx_len);
    *rx_len = state->rx_len;
    state->receive_rc = -(int)MESH_ERR_BUSY;
    return 0;
}

int mesh_uart_transport_init_default(void)
{
    return -(int)MESH_ERR_INVALID_STATE;
}

struct mesh_uart_transport *mesh_uart_transport_default(void)
{
    return NULL;
}

static void configure_port(
    struct mesh_host_service_config *config,
    size_t index,
    uint8_t neighbor)
{
    assert(index < MESH_HOST_SERVICE_MAX_PORTS);
    config->ports[index].enabled = true;
    config->ports[index].neighbor_addr = neighbor;
    config->ports[index].uart_config.uart = (void *)index;
}

static void test_single_port_sends_any_next_hop(void)
{
    struct mesh_host_service manager;
    struct mesh_host_service_config config;
    const uint8_t frame[] = {0x10u, 0x20u, 0x30u};

    fake_ports_reset();
    mesh_host_service_get_default_config(&config);
    config.ports[0].uart_config.uart = (void *)0u;

    assert(mesh_host_service_init(&manager, &config) == 0);
    assert(mesh_host_service_send_frame(&manager, 0x44u, frame, sizeof(frame)) == 0);
    assert(g_fake_ports[0].send_count == 1u);
    assert(g_fake_ports[0].last_tx_len == sizeof(frame));
    assert(memcmp(g_fake_ports[0].last_tx, frame, sizeof(frame)) == 0);
    mesh_host_service_deinit(&manager);
}

static void test_multi_port_sends_by_next_hop(void)
{
    struct mesh_host_service manager;
    struct mesh_host_service_config config;
    const uint8_t frame[] = {0xaau};

    fake_ports_reset();
    memset(&config, 0, sizeof(config));
    config.port_count = 2u;
    configure_port(&config, 0u, 0x11u);
    configure_port(&config, 1u, 0x33u);

    assert(mesh_host_service_init(&manager, &config) == 0);
    assert(mesh_host_service_send_frame(&manager, 0x33u, frame, sizeof(frame)) == 0);
    assert(g_fake_ports[0].send_count == 0u);
    assert(g_fake_ports[1].send_count == 1u);
    mesh_host_service_deinit(&manager);
}

static void test_unknown_next_hop_fails_on_multi_port(void)
{
    struct mesh_host_service manager;
    struct mesh_host_service_config config;
    const uint8_t frame[] = {0xaau};

    fake_ports_reset();
    memset(&config, 0, sizeof(config));
    config.port_count = 2u;
    configure_port(&config, 0u, 0x11u);
    configure_port(&config, 1u, 0x33u);

    assert(mesh_host_service_init(&manager, &config) == 0);
    assert(mesh_host_service_send_frame(&manager, 0x22u, frame, sizeof(frame)) == -(int)MESH_ERR_NO_ROUTE);
    assert(g_fake_ports[0].send_count == 0u);
    assert(g_fake_ports[1].send_count == 0u);
    mesh_host_service_deinit(&manager);
}

static void test_duplicate_neighbor_rejected(void)
{
    struct mesh_host_service manager;
    struct mesh_host_service_config config;

    fake_ports_reset();
    memset(&config, 0, sizeof(config));
    config.port_count = 2u;
    configure_port(&config, 0u, 0x11u);
    configure_port(&config, 1u, 0x11u);

    assert(mesh_host_service_init(&manager, &config) == -(int)MESH_ERR_INVALID_STATE);
}

static void test_receive_round_robin_scans_ports(void)
{
    struct mesh_host_service manager;
    struct mesh_host_service_config config;
    const uint8_t first[] = {0x01u};
    const uint8_t second[] = {0x02u};
    uint8_t rx[FAKE_RX_CAP];
    size_t rx_len = 0u;
    uint8_t ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;

    fake_ports_reset();
    memset(&config, 0, sizeof(config));
    config.port_count = 2u;
    configure_port(&config, 0u, 0x11u);
    configure_port(&config, 1u, 0x33u);

    assert(mesh_host_service_init(&manager, &config) == 0);
    fake_queue_rx(1u, first, sizeof(first));
    assert(mesh_host_service_receive_frame(&manager, rx, sizeof(rx), &rx_len, &ingress_port) == 0);
    assert(rx_len == sizeof(first));
    assert(rx[0] == first[0]);
    assert(ingress_port == 1u);
    assert(g_fake_ports[0].receive_count == 1u);
    assert(g_fake_ports[1].receive_count == 1u);

    fake_queue_rx(0u, second, sizeof(second));
    assert(mesh_host_service_receive_frame(&manager, rx, sizeof(rx), &rx_len, &ingress_port) == 0);
    assert(rx_len == sizeof(second));
    assert(rx[0] == second[0]);
    assert(ingress_port == 0u);
    mesh_host_service_deinit(&manager);
}

int main(void)
{
    test_single_port_sends_any_next_hop();
    test_multi_port_sends_by_next_hop();
    test_unknown_next_hop_fails_on_multi_port();
    test_duplicate_neighbor_rejected();
    test_receive_round_robin_scans_ports();
    printf("mesh_host_service tests passed\n");
    return 0;
}
