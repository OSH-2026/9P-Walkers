#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../node_runtime/mesh_node_runtime.h"
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

static int fake_receive_frame(void *transport_ctx, uint8_t *rx_data, size_t rx_cap, size_t *rx_len)
{
    struct fake_transport *transport = (struct fake_transport *)transport_ctx;

    assert(transport != NULL);
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
    uint8_t request_frame[96];
    uint8_t mesh_frame[TEST_FRAME_CAP];
    size_t assign_len = 0u;
    size_t request_len = 0u;
    size_t mesh_len = 0u;
    uint8_t next_hop = 0u;
    bool is_local = false;
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
        0x01u,
        MESH_ADDR_UNASSIGNED,
        0x1001u,
        6u,
        &assign_payload,
        assign_frame,
        sizeof(assign_frame),
        &assign_len));
    fake_transport_queue_rx(&transport, assign_frame, assign_len);
    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(runtime.cluster.config.local_addr == 0x24u);
    assert(runtime.processor.config.local_addr == 0x24u);
    assert(transport.tx_count == 1u);
    assert(transport.tx_next_hop[0] == 0x01u);
    assert(mesh_decode_frame(transport.tx_queue[0].data, transport.tx_queue[0].len, &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(view.src == 0x24u);
    assert(view.dst == MESH_ADDR_UNASSIGNED);
    assert(mesh_parse_register(&view, &register_payload));
    assert(memcmp(register_payload.uid, uid, sizeof(uid)) == 0);
    assert(cluster_lookup_next_hop(&runtime.cluster, 0x01u, &next_hop, &is_local) == 0);
    assert(!is_local);
    assert(next_hop == 0x01u);

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
    fake_transport_queue_rx(&transport, mesh_frame, mesh_len);

    assert(mesh_node_runtime_poll_once(&runtime) == 0);
    assert(server_ctx.call_count == 1u);
    assert(server_ctx.last_tag == 0x3344u);
    assert(transport.tx_count == 2u);
    assert(transport.tx_next_hop[1] == 0x01u);
    assert(mesh_decode_frame(transport.tx_queue[1].data, transport.tx_queue[1].len, &view));
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

int main(void)
{
    test_auto_register_on_init_sends_uid_to_current_link();
    test_assign_updates_local_addr_and_allows_server_reply();
    test_assign_with_foreign_uid_is_ignored();

    puts("mesh node runtime host tests passed");
    return 0;
}