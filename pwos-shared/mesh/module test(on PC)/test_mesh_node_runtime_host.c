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
    uint8_t ingress_port;
};

struct fake_transport {
    struct fake_frame rx_queue[8];
    size_t rx_count;
    size_t rx_index;
    struct fake_frame tx_queue[8];
    uint8_t tx_next_hop[8];
    size_t tx_count;
    uint8_t learned_addr[8];
    uint8_t learned_port[8];
    size_t learned_count;
};

struct fake_server_ctx {
    uint16_t last_tag;
    uint8_t call_count;
};

static void fake_transport_reset(struct fake_transport *transport)
{
    memset(transport, 0, sizeof(*transport));
}

static void fake_transport_queue_rx_on_port(
    struct fake_transport *transport,
    const uint8_t *data,
    size_t len,
    uint8_t ingress_port)
{
    assert(transport->rx_count < (sizeof(transport->rx_queue) / sizeof(transport->rx_queue[0])));
    assert(len <= sizeof(transport->rx_queue[transport->rx_count].data));

    memcpy(transport->rx_queue[transport->rx_count].data, data, len);
    transport->rx_queue[transport->rx_count].len = len;
    transport->rx_queue[transport->rx_count].ingress_port = ingress_port;
    ++transport->rx_count;
}

static void fake_transport_queue_rx(struct fake_transport *transport, const uint8_t *data, size_t len)
{
    fake_transport_queue_rx_on_port(transport, data, len, MESH_PROCESSER_INGRESS_PORT_NONE);
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

static int fake_send_frame_to_port(void *transport_ctx, uint8_t port_id, const uint8_t *tx_data, size_t tx_len)
{
    return fake_send_frame(transport_ctx, port_id, tx_data, tx_len);
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
    if (transport->rx_index >= transport->rx_count) {
        return -(int)MESH_ERR_BUSY;
    }
    assert(transport->rx_queue[transport->rx_index].len <= rx_cap);

    memcpy(rx_data,
           transport->rx_queue[transport->rx_index].data,
           transport->rx_queue[transport->rx_index].len);
    *rx_len = transport->rx_queue[transport->rx_index].len;
    *out_ingress_port = transport->rx_queue[transport->rx_index].ingress_port;
    ++transport->rx_index;
    return 0;
}

static int fake_learn_peer_port(void *ctx, uint8_t mesh_addr, uint8_t port_id)
{
    struct fake_transport *transport = (struct fake_transport *)ctx;

    assert(transport != NULL);
    assert(transport->learned_count < (sizeof(transport->learned_addr) / sizeof(transport->learned_addr[0])));

    transport->learned_addr[transport->learned_count] = mesh_addr;
    transport->learned_port[transport->learned_count] = port_id;
    ++transport->learned_count;
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

static size_t find_tx_frame_type(const struct fake_transport *transport, uint8_t type)
{
    size_t i;

    for (i = 0u; i < transport->tx_count; ++i) {
        struct mesh_frame_view view;

        if (mesh_decode_frame(transport->tx_queue[i].data, transport->tx_queue[i].len, &view) &&
            view.type == type) {
            return i;
        }
    }

    return transport->tx_count;
}

static void init_runtime(
    struct mesh_node_runtime *runtime,
    struct fake_transport *transport,
    struct fake_server_ctx *server_ctx,
    const uint8_t uid[MESH_UID_LEN],
    bool auto_register_on_init)
{
    struct mesh_node_runtime_config config;

    mesh_node_runtime_get_default_config(&config);
    config.send_frame = fake_send_frame;
    config.receive_frame = fake_receive_frame;
    config.transport_ctx = transport;
    config.learn_peer_port = fake_learn_peer_port;
    config.learn_peer_port_ctx = transport;
    config.mini9p_server_handler = fake_mini9p_server_handler;
    config.mini9p_server_ctx = server_ctx;
    memcpy(config.local_uid, uid, MESH_UID_LEN);
    config.boot_nonce = 0x12345678u;
    config.capability_bits = 0x0001u;
    config.port_bitmap = 0x02u;
    config.bootstrap_next_hop = 0x31u;
    config.auto_register_on_init = auto_register_on_init;

    assert(mesh_node_runtime_init(runtime, &config) == 0);
}

static void test_auto_register_on_init_sends_uid_to_current_link(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct mesh_frame_view view;
    struct mesh_register_payload payload;
    const uint8_t uid[MESH_UID_LEN] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u};

    memset(&server_ctx, 0, sizeof(server_ctx));
    fake_transport_reset(&transport);

    init_runtime(&runtime, &transport, &server_ctx, uid, true);

    assert(transport.tx_count == 1u);
    assert(transport.tx_next_hop[0] == 0x31u);
    assert(mesh_decode_frame(transport.tx_queue[0].data, transport.tx_queue[0].len, &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(view.src == MESH_ADDR_UNASSIGNED);
    assert(view.dst == MESH_ADDR_UNASSIGNED);
    assert(mesh_parse_register(&view, &payload));
    assert(memcmp(payload.uid, uid, sizeof(uid)) == 0);
    assert(payload.boot_nonce == 0x12345678u);
    assert(payload.capability_bits == 0x0001u);
    assert(payload.port_bitmap == 0x02u);

    mesh_node_runtime_deinit(&runtime);
}

static void test_assign_updates_local_addr_and_allows_server_reply(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct mesh_assign_payload assign_payload;
    struct mesh_frame_view view;
    struct mesh_register_payload register_payload;
    struct m9p_frame_view mini9p_view;
    uint8_t assign_frame[160];
    uint8_t probe_response_frame[64];
    uint8_t request_frame[96];
    uint8_t mesh_frame[TEST_FRAME_CAP];
    size_t assign_len = 0u;
    size_t probe_response_len = 0u;
    size_t request_len = 0u;
    size_t mesh_len = 0u;
    uint8_t next_hop = 0u;
    bool is_local = false;
    size_t register_tx_index;
    size_t mini9p_tx_index;
    size_t tx_count_before_request;
    const uint8_t uid[MESH_UID_LEN] = {0xA1u, 0xA2u, 0xA3u, 0xA4u, 0xA5u, 0xA6u, 0xA7u, 0xA8u};

    memset(&server_ctx, 0, sizeof(server_ctx));
    fake_transport_reset(&transport);

    init_runtime(&runtime, &transport, &server_ctx, uid, false);

    memset(&assign_payload, 0, sizeof(assign_payload));
    memcpy(assign_payload.uid, uid, sizeof(uid));
    assign_payload.node_addr = 0x24u;
    assign_payload.lease_ms = 30000u;
    assign_payload.epoch = 7u;
    memcpy(assign_payload.node_name, "mcu1", 5u);

    assert(mesh_build_assign(
        0x00u,
        MESH_ADDR_UNASSIGNED,
        0x1001u,
        6u,
        &assign_payload,
        assign_frame,
        sizeof(assign_frame),
        &assign_len));
    fake_transport_queue_rx_on_port(&transport, assign_frame, assign_len, 2u);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(runtime.cluster.config.local_addr == 0x24u);
    assert(runtime.processor.config.local_addr == 0x24u);
    register_tx_index = find_tx_frame_type(&transport, MESH_TYPE_REGISTER);
    assert(register_tx_index < transport.tx_count);
    assert(transport.tx_next_hop[register_tx_index] == 0x00u);
    assert(mesh_decode_frame(
        transport.tx_queue[register_tx_index].data,
        transport.tx_queue[register_tx_index].len,
        &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(view.src == 0x24u);
    assert(view.dst == MESH_ADDR_UNASSIGNED);
    assert(mesh_parse_register(&view, &register_payload));
    assert(memcmp(register_payload.uid, uid, sizeof(uid)) == 0);
    assert(find_tx_frame_type(&transport, MESH_TYPE_NEIGHBOR_PROBE_REQUEST) < transport.tx_count);

    assert(mesh_build_neighbor_probe_response(
        0x01u,
        MESH_ADDR_UNASSIGNED,
        0x1002u,
        6u,
        probe_response_frame,
        sizeof(probe_response_frame),
        &probe_response_len));
    fake_transport_queue_rx_on_port(&transport, probe_response_frame, probe_response_len, 2u);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x01u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 0x01u);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x00u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 0x01u);
    assert(transport.learned_count == 1u);
    assert(transport.learned_addr[0] == 0x01u);
    assert(transport.learned_port[0] == 2u);
    tx_count_before_request = transport.tx_count;

    assert(m9p_build_tclunk(0x3344u, 9u, request_frame, sizeof(request_frame), &request_len));
    assert(mesh_build_mini9p_frame(
        0x00u,
        0x24u,
        0x1002u,
        6u,
        0u,
        request_frame,
        (uint16_t)request_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));
    fake_transport_queue_rx_on_port(&transport, mesh_frame, mesh_len, 2u);

    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(server_ctx.call_count == 1u);
    assert(server_ctx.last_tag == 0x3344u);
    assert(transport.tx_count == tx_count_before_request + 1u);
    mini9p_tx_index = transport.tx_count - 1u;
    assert(transport.tx_next_hop[mini9p_tx_index] == 0x01u);
    assert(transport.learned_count == 1u);
    assert(mesh_decode_frame(
        transport.tx_queue[mini9p_tx_index].data,
        transport.tx_queue[mini9p_tx_index].len,
        &view));
    assert(view.type == MESH_TYPE_MINI9P);
    assert(view.src == 0x24u);
    assert(view.dst == 0x00u);
    assert(m9p_decode_frame(view.payload, view.payload_len, &mini9p_view));
    assert((mini9p_view.type & 0x80u) != 0u);
    assert(mini9p_view.tag == 0x3344u);

    mesh_node_runtime_deinit(&runtime);
}

static void test_assign_with_foreign_uid_is_ignored(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct mesh_assign_payload assign_payload;
    uint8_t assign_frame[160];
    size_t assign_len = 0u;
    const uint8_t local_uid[MESH_UID_LEN] = {0xB1u, 0xB2u, 0xB3u, 0xB4u, 0xB5u, 0xB6u, 0xB7u, 0xB8u};
    const uint8_t foreign_uid[MESH_UID_LEN] = {0xC1u, 0xC2u, 0xC3u, 0xC4u, 0xC5u, 0xC6u, 0xC7u, 0xC8u};

    memset(&server_ctx, 0, sizeof(server_ctx));
    fake_transport_reset(&transport);

    init_runtime(&runtime, &transport, &server_ctx, local_uid, false);

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
    fake_transport_queue_rx(&transport, assign_frame, assign_len);

    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(runtime.cluster.config.local_addr == MESH_ADDR_UNASSIGNED);
    assert(runtime.processor.config.local_addr == MESH_ADDR_UNASSIGNED);

    mesh_node_runtime_deinit(&runtime);
}

static void test_neighbor_probe_request_learns_parent_and_reports_link(void)
{
    struct mesh_node_runtime runtime;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    uint8_t probe_request_frame[64];
    size_t probe_request_len = 0u;
    uint8_t next_hop = 0u;
    bool is_local = true;
    size_t link_state_index;
    size_t probe_response_index;
    struct mesh_frame_view view;
    struct mesh_link_state_payload payload;
    const uint8_t uid[MESH_UID_LEN] = {0xD1u, 0xD2u, 0xD3u, 0xD4u, 0xD5u, 0xD6u, 0xD7u, 0xD8u};

    memset(&server_ctx, 0, sizeof(server_ctx));
    fake_transport_reset(&transport);

    init_runtime(&runtime, &transport, &server_ctx, uid, false);
    runtime.config.send_frame_to_port = fake_send_frame_to_port;
    runtime.initialized = true;
    runtime.cluster.config.local_addr = 0x22u;
    runtime.processor.config.local_addr = 0x22u;
    runtime.upstream_port = 2u;
    runtime.control_plane_addr = 0x00u;

    assert(mesh_build_neighbor_probe_request(
        0x11u,
        0x2001u,
        6u,
        probe_request_frame,
        sizeof(probe_request_frame),
        &probe_request_len));
    fake_transport_queue_rx_on_port(&transport, probe_request_frame, probe_request_len, 2u);

    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x11u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 0x11u);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x00u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 0x11u);
    assert(transport.learned_count == 1u);
    assert(transport.learned_addr[0] == 0x11u);
    assert(transport.learned_port[0] == 2u);

    link_state_index = find_tx_frame_type(&transport, MESH_TYPE_LINK_STATE);
    assert(link_state_index < transport.tx_count);
    assert(mesh_decode_frame(
        transport.tx_queue[link_state_index].data,
        transport.tx_queue[link_state_index].len,
        &view));
    assert(view.src == 0x22u);
    assert(view.dst == 0x00u);
    assert(mesh_parse_link_state(&view, &payload));
    assert(payload.neighbor == 0x11u);
    assert(payload.link_up == 1u);

    probe_response_index = find_tx_frame_type(&transport, MESH_TYPE_NEIGHBOR_PROBE_RESPONSE);
    assert(probe_response_index < transport.tx_count);
    assert(transport.tx_next_hop[probe_response_index] == 2u);

    mesh_node_runtime_deinit(&runtime);
}

int main(void)
{
    test_auto_register_on_init_sends_uid_to_current_link();
    test_assign_updates_local_addr_and_allows_server_reply();
    test_assign_with_foreign_uid_is_ignored();
    test_neighbor_probe_request_learns_parent_and_reports_link();

    puts("mesh node runtime host tests passed");
    return 0;
}
