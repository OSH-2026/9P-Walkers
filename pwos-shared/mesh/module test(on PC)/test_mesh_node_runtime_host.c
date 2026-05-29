#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../pwos-slave/User/mesh/mesh_node_runtime.h"
#include "../../mini9p/mini9p_protocol.h"

#define TEST_FRAME_CAP (MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN)

struct fake_frame {
    uint8_t data[TEST_FRAME_CAP];
    size_t len;
};

struct fake_transport {
    struct fake_frame rx_queue[8];
    size_t rx_count;
    size_t rx_index;
    struct fake_frame tx_queue[8];
    uint8_t tx_next_hop[8];
    size_t tx_count;
};

struct fake_server_ctx {
    uint16_t last_tag;
    uint8_t call_count;
};

static void fake_transport_reset(struct fake_transport *transport)
{
    memset(transport, 0, sizeof(*transport));
}

static void fake_transport_queue_rx(struct fake_transport *transport, const uint8_t *data, size_t len)
{
    assert(transport->rx_count < (sizeof(transport->rx_queue) / sizeof(transport->rx_queue[0])));
    assert(len <= sizeof(transport->rx_queue[transport->rx_count].data));

    memcpy(transport->rx_queue[transport->rx_count].data, data, len);
    transport->rx_queue[transport->rx_count].len = len;
    ++transport->rx_count;
}

static int fake_send_frame(void *transport_ctx, uint8_t next_hop, const uint8_t *tx_data, size_t tx_len)
{
    struct fake_transport *transport = (struct fake_transport *)transport_ctx;

    assert(transport != NULL);
    assert(transport->tx_count < (sizeof(transport->tx_queue) / sizeof(transport->tx_queue[0])));
    assert(tx_len <= sizeof(transport->tx_queue[transport->tx_count].data));

    memcpy(transport->tx_queue[transport->tx_count].data, tx_data, tx_len);
    transport->tx_queue[transport->tx_count].len = tx_len;
    transport->tx_next_hop[transport->tx_count] = next_hop;
    ++transport->tx_count;
    return 0;
}

static int fake_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len,
    uint8_t *out_ingress_port)
{
    struct fake_transport *transport = (struct fake_transport *)transport_ctx;

    assert(transport != NULL);
    assert(out_ingress_port != NULL);
    *out_ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;
    if (transport->rx_index >= transport->rx_count) {
        return -(int)MESH_ERR_BUSY;
    }
    assert(transport->rx_queue[transport->rx_index].len <= rx_cap);

    memcpy(rx_data,
           transport->rx_queue[transport->rx_index].data,
           transport->rx_queue[transport->rx_index].len);
    *rx_len = transport->rx_queue[transport->rx_index].len;
    ++transport->rx_index;
    return 0;
}

static int fake_mini9p_server_handler(
    void *server_ctx,
    const uint8_t *request_data,
    size_t request_len,
    uint8_t *out_response_data,
    size_t response_cap,
    size_t *out_response_len)
{
    struct fake_server_ctx *ctx = (struct fake_server_ctx *)server_ctx;
    struct m9p_frame_view request_view;

    assert(ctx != NULL);
    assert(m9p_decode_frame(request_data, request_len, &request_view));

    ctx->last_tag = request_view.tag;
    ++ctx->call_count;

    return m9p_build_rclunk(request_view.tag, out_response_data, response_cap, out_response_len)
               ? 0
               : -(int)MESH_ERR_BAD_FRAME;
}

static void init_runtime(
    struct mesh_node_runtime *runtime,
    struct fake_transport *transports,
    const uint8_t *port_ids,
    size_t port_count,
    struct fake_server_ctx *server_ctx,
    const uint8_t uid[MESH_UID_LEN],
    bool auto_register_on_init)
{
    struct mesh_node_runtime_config config;
    struct mesh_node_runtime_port_config port_configs[MESH_NODE_RUNTIME_MAX_PORTS];
    size_t i;

    assert(port_count <= MESH_NODE_RUNTIME_MAX_UART_PORTS);

    mesh_node_runtime_get_default_config(&config);
    memset(port_configs, 0, sizeof(port_configs));
    for (i = 0u; i < port_count; ++i) {
        port_configs[i].send_frame = fake_send_frame;
        port_configs[i].receive_frame = fake_receive_frame;
        port_configs[i].transport_ctx = &transports[i];
        port_configs[i].port_id = port_ids[i];
    }

    config.ports = port_configs;
    config.mini9p_server_handler = fake_mini9p_server_handler;
    config.mini9p_server_ctx = server_ctx;
    memcpy(config.local_uid, uid, MESH_UID_LEN);
    config.boot_nonce = 0x12345678u;
    config.capability_bits = 0x0001u;
    config.port_bitmap = 0u;
    config.auto_register_on_init = auto_register_on_init;

    assert(mesh_node_runtime_init(runtime, &config, port_count) == 0);
}

static void test_auto_register_on_init_sends_uid_to_every_bound_port(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transports[2];
    struct fake_server_ctx server_ctx;
    struct mesh_frame_view view;
    struct mesh_register_payload payload;
    const uint8_t uid[MESH_UID_LEN] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u};
    const uint8_t port_ids[2] = {0u, 1u};
    size_t i;

    memset(&server_ctx, 0, sizeof(server_ctx));
    for (i = 0u; i < 2u; ++i) {
        fake_transport_reset(&transports[i]);
    }

    init_runtime(&runtime, transports, port_ids, 2u, &server_ctx, uid, true);

    assert(transports[0].tx_count == 1u);
    assert(transports[1].tx_count == 1u);
    assert(transports[0].tx_next_hop[0] == 0u);
    assert(transports[1].tx_next_hop[0] == 1u);
    assert(mesh_decode_frame(transports[1].tx_queue[0].data, transports[1].tx_queue[0].len, &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(view.src == MESH_ADDR_UNASSIGNED);
    assert(view.dst == MESH_ADDR_UNASSIGNED);
    assert(mesh_parse_register(&view, &payload));
    assert(memcmp(payload.uid, uid, sizeof(uid)) == 0);
    assert(payload.boot_nonce == 0x12345678u);
    assert(payload.capability_bits == 0x0001u);
    assert(payload.port_bitmap == 0x03u);

    mesh_node_runtime_deinit(&runtime);
}

static void test_assign_updates_local_addr_and_allows_server_reply(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transports[1];
    struct fake_server_ctx server_ctx;
    struct mesh_assign_payload assign_payload;
    struct mesh_link_state_payload link_payload;
    struct mesh_frame_view view;
    struct mesh_register_payload register_payload;
    struct m9p_frame_view mini9p_view;
    uint8_t assign_frame[160];
    uint8_t request_frame[96];
    uint8_t mesh_frame[TEST_FRAME_CAP];
    size_t assign_len = 0u;
    size_t request_len = 0u;
    size_t mesh_len = 0u;
    uint8_t next_hop = 0u;
    bool is_local = false;
    const uint8_t uid[MESH_UID_LEN] = {0xA1u, 0xA2u, 0xA3u, 0xA4u, 0xA5u, 0xA6u, 0xA7u, 0xA8u};
    const uint8_t port_ids[1] = {1u};

    memset(&server_ctx, 0, sizeof(server_ctx));
    fake_transport_reset(&transports[0]);

    init_runtime(&runtime, transports, port_ids, 1u, &server_ctx, uid, false);

    memset(&assign_payload, 0, sizeof(assign_payload));
    memcpy(assign_payload.uid, uid, sizeof(uid));
    assign_payload.node_addr = 0x24u;
    assign_payload.lease_ms = 30000u;
    assign_payload.epoch = 7u;
    memcpy(assign_payload.node_name, "mcu1", 5u);

    assert(mesh_build_assign(
        0x01u,
        MESH_ADDR_UNASSIGNED,
        0x1001u,
        6u,
        &assign_payload,
        assign_frame,
        sizeof(assign_frame),
        &assign_len));
    fake_transport_queue_rx(&transports[0], assign_frame, assign_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(runtime.cluster.config.local_addr == 0x24u);
    assert(runtime.processor.config.local_addr == 0x24u);
    assert(transports[0].tx_count == 2u);
    assert(transports[0].tx_next_hop[0] == 1u);
    assert(mesh_decode_frame(transports[0].tx_queue[0].data, transports[0].tx_queue[0].len, &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(view.src == 0x24u);
    assert(view.dst == MESH_ADDR_UNASSIGNED);
    assert(mesh_parse_register(&view, &register_payload));
    assert(memcmp(register_payload.uid, uid, sizeof(uid)) == 0);
    assert(mesh_decode_frame(transports[0].tx_queue[1].data, transports[0].tx_queue[1].len, &view));
    assert(view.type == MESH_TYPE_LINK_STATE);
    assert(view.dst == 0x00u);
    assert(mesh_parse_link_state(&view, &link_payload));
    assert(link_payload.neighbor == 0x01u);
    assert(link_payload.local_port == 1u);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x01u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 1u);

    assert(m9p_build_tclunk(0x3344u, 9u, request_frame, sizeof(request_frame), &request_len));
    assert(mesh_build_mini9p_frame(
        0x01u,
        0x24u,
        0x1002u,
        6u,
        0u,
        request_frame,
        (uint16_t)request_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));
    fake_transport_queue_rx(&transports[0], mesh_frame, mesh_len);

    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(server_ctx.call_count == 1u);
    assert(server_ctx.last_tag == 0x3344u);
    assert(transports[0].tx_count == 3u);
    assert(transports[0].tx_next_hop[2] == 1u);
    assert(mesh_decode_frame(transports[0].tx_queue[2].data, transports[0].tx_queue[2].len, &view));
    assert(view.type == MESH_TYPE_MINI9P);
    assert(view.src == 0x24u);
    assert(view.dst == 0x01u);
    assert(m9p_decode_frame(view.payload, view.payload_len, &mini9p_view));
    assert((mini9p_view.type & 0x80u) != 0u);
    assert(mini9p_view.tag == 0x3344u);

    mesh_node_runtime_deinit(&runtime);
}

static void test_assign_with_foreign_uid_is_ignored(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transports[1];
    struct fake_server_ctx server_ctx;
    struct mesh_assign_payload assign_payload;
    uint8_t assign_frame[160];
    size_t assign_len = 0u;
    const uint8_t local_uid[MESH_UID_LEN] = {0xB1u, 0xB2u, 0xB3u, 0xB4u, 0xB5u, 0xB6u, 0xB7u, 0xB8u};
    const uint8_t foreign_uid[MESH_UID_LEN] = {0xC1u, 0xC2u, 0xC3u, 0xC4u, 0xC5u, 0xC6u, 0xC7u, 0xC8u};
    const uint8_t port_ids[1] = {0u};

    memset(&server_ctx, 0, sizeof(server_ctx));
    fake_transport_reset(&transports[0]);

    init_runtime(&runtime, transports, port_ids, 1u, &server_ctx, local_uid, false);

    memset(&assign_payload, 0, sizeof(assign_payload));
    memcpy(assign_payload.uid, foreign_uid, sizeof(foreign_uid));
    assign_payload.node_addr = 0x35u;
    assign_payload.lease_ms = 12000u;
    assign_payload.epoch = 9u;
    memcpy(assign_payload.node_name, "mcuX", 5u);

    assert(mesh_build_assign(
        0x01u,
        MESH_ADDR_UNASSIGNED,
        0x1003u,
        6u,
        &assign_payload,
        assign_frame,
        sizeof(assign_frame),
        &assign_len));
    fake_transport_queue_rx(&transports[0], assign_frame, assign_len);

    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(runtime.cluster.config.local_addr == MESH_ADDR_UNASSIGNED);
    assert(runtime.processor.config.local_addr == MESH_ADDR_UNASSIGNED);
    assert(transports[0].tx_count == 0u);

    mesh_node_runtime_deinit(&runtime);
}

static void test_multi_port_forward_uses_route_selected_egress(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transports[2];
    struct fake_server_ctx server_ctx;
    struct mesh_assign_payload assign_payload;
    struct mesh_register_payload register_payload;
    struct mesh_route_update_payload route_payload;
    struct mesh_ping_payload ping_payload;
    struct mesh_frame_view view;
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;
    uint8_t next_hop = 0u;
    bool is_local = false;
    const uint8_t uid[MESH_UID_LEN] = {0xD1u, 0xD2u, 0xD3u, 0xD4u, 0xD5u, 0xD6u, 0xD7u, 0xD8u};
    const uint8_t child_uid[MESH_UID_LEN] = {0xE1u, 0xE2u, 0xE3u, 0xE4u, 0xE5u, 0xE6u, 0xE7u, 0xE8u};
    const uint8_t port_ids[2] = {0u, 1u};
    struct mesh_link_state_payload link_payload;
    size_t i;

    memset(&server_ctx, 0, sizeof(server_ctx));
    for (i = 0u; i < 2u; ++i) {
        fake_transport_reset(&transports[i]);
    }

    init_runtime(&runtime, transports, port_ids, 2u, &server_ctx, uid, false);

    memset(&assign_payload, 0, sizeof(assign_payload));
    memcpy(assign_payload.uid, uid, sizeof(uid));
    assign_payload.node_addr = 0x24u;
    assert(mesh_build_assign(0x01u, MESH_ADDR_UNASSIGNED, 0x2001u, 6u, &assign_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);

    memset(&register_payload, 0, sizeof(register_payload));
    memcpy(register_payload.uid, child_uid, sizeof(child_uid));
    register_payload.boot_nonce = 0xABCDEF01u;
    register_payload.capability_bits = 0x0001u;
    register_payload.port_bitmap = 0x01u;
    assert(mesh_build_register(0x31u, 0x2002u, 6u, &register_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[1], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x31u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 1u);
    assert(transports[0].tx_count == 3u);
    assert(mesh_decode_frame(transports[0].tx_queue[2].data, transports[0].tx_queue[2].len, &view));
    assert(view.type == MESH_TYPE_LINK_STATE);
    assert(mesh_parse_link_state(&view, &link_payload));
    assert(link_payload.neighbor == 0x31u);
    assert(link_payload.local_port == 1u);

    memset(&route_payload, 0, sizeof(route_payload));
    route_payload.dst = 0x41u;
    route_payload.next_hop = 1u;
    route_payload.metric = 1u;
    route_payload.route_version = 2u;
    route_payload.action = MESH_ROUTE_SET;
    assert(mesh_build_route_update(0x01u, 0x24u, 0x2003u, 6u, &route_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x41u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 1u);

    memset(&ping_payload, 0, sizeof(ping_payload));
    ping_payload.local_time_ms = 0x55667788u;
    assert(mesh_build_ping(0x01u, 0x41u, 0x2004u, 6u, MESH_TYPE_PING, &ping_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(transports[1].tx_count == 1u);
    assert(transports[1].tx_next_hop[0] == 1u);
    assert(mesh_decode_frame(transports[1].tx_queue[0].data, transports[1].tx_queue[0].len, &view));
    assert(view.type == MESH_TYPE_PING);
    assert(view.dst == 0x41u);
    assert(view.hop == 5u);

    mesh_node_runtime_deinit(&runtime);
}

static void test_wifi_transport_register_and_forward_use_reserved_port(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transports[1];
    struct fake_transport wifi_transport;
    struct fake_server_ctx server_ctx;
    struct mesh_assign_payload assign_payload;
    struct mesh_register_payload register_payload;
    struct mesh_route_update_payload route_payload;
    struct mesh_ping_payload ping_payload;
    struct mesh_frame_view view;
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;
    uint8_t next_hop = 0u;
    bool is_local = false;
    const uint8_t uid[MESH_UID_LEN] = {0x91u, 0x92u, 0x93u, 0x94u, 0x95u, 0x96u, 0x97u, 0x98u};
    const uint8_t port_ids[1] = {0u};

    memset(&server_ctx, 0, sizeof(server_ctx));
    fake_transport_reset(&transports[0]);
    fake_transport_reset(&wifi_transport);

    init_runtime(&runtime, transports, port_ids, 1u, &wifi_transport, true, &server_ctx, uid, true);

    assert(transports[0].tx_count == 1u);
    assert(wifi_transport.tx_count == 1u);
    assert(wifi_transport.tx_next_hop[0] == CLUSTER_PORT_WIFI_ID);
    assert(mesh_decode_frame(wifi_transport.tx_queue[0].data, wifi_transport.tx_queue[0].len, &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(mesh_parse_register(&view, &register_payload));
    assert(register_payload.wifi_supported);
    assert(register_payload.port_bitmap == (uint8_t)(0x01u | CLUSTER_PORT_WIFI_MASK));

    memset(&assign_payload, 0, sizeof(assign_payload));
    memcpy(assign_payload.uid, uid, sizeof(uid));
    assign_payload.node_addr = 0x24u;
    assert(mesh_build_assign(0x01u, MESH_ADDR_UNASSIGNED, 0x3001u, 6u, &assign_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);

    memset(&route_payload, 0, sizeof(route_payload));
    route_payload.dst = 0x66u;
    route_payload.next_hop = CLUSTER_PORT_WIFI_ID;
    route_payload.metric = 1u;
    route_payload.route_version = 3u;
    route_payload.action = MESH_ROUTE_SET;
    assert(mesh_build_route_update(0x01u, 0x24u, 0x3002u, 6u, &route_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x66u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == CLUSTER_PORT_WIFI_ID);

    memset(&ping_payload, 0, sizeof(ping_payload));
    ping_payload.local_time_ms = 0x01020304u;
    assert(mesh_build_ping(0x01u, 0x66u, 0x3003u, 6u, MESH_TYPE_PING, &ping_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(wifi_transport.tx_count == 2u);
    assert(wifi_transport.tx_next_hop[1] == CLUSTER_PORT_WIFI_ID);
    assert(mesh_decode_frame(wifi_transport.tx_queue[1].data, wifi_transport.tx_queue[1].len, &view));
    assert(view.type == MESH_TYPE_PING);
    assert(view.dst == 0x66u);

    mesh_node_runtime_deinit(&runtime);
}

static void test_mixed_wifi_and_uart_routes_choose_expected_egress(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transports[1];
    struct fake_transport wifi_transport;
    struct fake_server_ctx server_ctx;
    struct mesh_assign_payload assign_payload;
    struct mesh_register_payload register_payload;
    struct mesh_route_update_payload route_payload;
    struct mesh_ping_payload ping_payload;
    struct mesh_frame_view view;
    uint8_t frame[TEST_FRAME_CAP];
    size_t frame_len = 0u;
    uint8_t next_hop = 0u;
    bool is_local = false;
    const uint8_t uid[MESH_UID_LEN] = {0xA9u, 0xAAu, 0xABu, 0xACu, 0xADu, 0xAEu, 0xAFu, 0xB0u};
    const uint8_t port_ids[1] = {1u};

    memset(&server_ctx, 0, sizeof(server_ctx));
    fake_transport_reset(&transports[0]);
    fake_transport_reset(&wifi_transport);

    init_runtime(&runtime, transports, port_ids, 1u, &wifi_transport, true, &server_ctx, uid, true);

    assert(transports[0].tx_count == 1u);
    assert(wifi_transport.tx_count == 1u);
    assert(mesh_decode_frame(transports[0].tx_queue[0].data, transports[0].tx_queue[0].len, &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(mesh_parse_register(&view, &register_payload));
    assert(register_payload.wifi_supported);
    assert(register_payload.port_bitmap == (uint8_t)((1u << 1u) | CLUSTER_PORT_WIFI_MASK));

    memset(&assign_payload, 0, sizeof(assign_payload));
    memcpy(assign_payload.uid, uid, sizeof(uid));
    assign_payload.node_addr = 0x24u;
    assert(mesh_build_assign(0x01u, MESH_ADDR_UNASSIGNED, 0x3101u, 6u, &assign_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);

    memset(&route_payload, 0, sizeof(route_payload));
    route_payload.dst = 0x41u;
    route_payload.next_hop = 1u;
    route_payload.metric = 1u;
    route_payload.route_version = 5u;
    route_payload.action = MESH_ROUTE_SET;
    assert(mesh_build_route_update(0x01u, 0x24u, 0x3102u, 6u, &route_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);

    memset(&route_payload, 0, sizeof(route_payload));
    route_payload.dst = 0x66u;
    route_payload.next_hop = CLUSTER_PORT_WIFI_ID;
    route_payload.metric = 1u;
    route_payload.route_version = 6u;
    route_payload.action = MESH_ROUTE_SET;
    assert(mesh_build_route_update(0x01u, 0x24u, 0x3103u, 6u, &route_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);

    assert(cluster_lookup_next_hop(&runtime.cluster, 0x41u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 1u);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x66u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == CLUSTER_PORT_WIFI_ID);

    memset(&ping_payload, 0, sizeof(ping_payload));
    ping_payload.local_time_ms = 0x11121314u;
    assert(mesh_build_ping(0x01u, 0x41u, 0x3104u, 6u, MESH_TYPE_PING, &ping_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(transports[0].tx_count == 4u);
    assert(transports[0].tx_next_hop[3] == 1u);
    assert(mesh_decode_frame(transports[0].tx_queue[3].data, transports[0].tx_queue[3].len, &view));
    assert(view.type == MESH_TYPE_PING);
    assert(view.dst == 0x41u);

    memset(&ping_payload, 0, sizeof(ping_payload));
    ping_payload.local_time_ms = 0x21222324u;
    assert(mesh_build_ping(0x01u, 0x66u, 0x3105u, 6u, MESH_TYPE_PING, &ping_payload, frame, sizeof(frame), &frame_len));
    fake_transport_queue_rx(&transports[0], frame, frame_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(wifi_transport.tx_count == 2u);
    assert(wifi_transport.tx_next_hop[1] == CLUSTER_PORT_WIFI_ID);
    assert(mesh_decode_frame(wifi_transport.tx_queue[1].data, wifi_transport.tx_queue[1].len, &view));
    assert(view.type == MESH_TYPE_PING);
    assert(view.dst == 0x66u);

    mesh_node_runtime_deinit(&runtime);
}

int main(void)
{
    test_auto_register_on_init_sends_uid_to_every_bound_port();
    test_assign_updates_local_addr_and_allows_server_reply();
    test_assign_with_foreign_uid_is_ignored();
    test_multi_port_forward_uses_route_selected_egress();
    test_wifi_transport_register_and_forward_use_reserved_port();
    test_mixed_wifi_and_uart_routes_choose_expected_egress();

    puts("mesh node runtime host tests passed");
    return 0;
}
