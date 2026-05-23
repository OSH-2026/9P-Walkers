#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../cluster/cluster.h"
#include "../envelope/mesh_protocal.h"
#include "../processer/mesh_processer.h"
#include "../../mini9p/mini9p_protocol.h"

struct fake_frame {
    uint8_t data[768];
    size_t len;
};

struct fake_transport {
    struct fake_frame rx_queue[16];
    size_t rx_count;
    size_t rx_index;

    struct fake_frame tx_queue[16];
    uint8_t tx_next_hop[16];
    size_t tx_count;
};

struct fake_server_ctx {
    uint16_t last_tag;
    uint8_t call_count;
};

struct fake_client_ctx {
    uint16_t last_tag;
    uint8_t last_type;
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
    if (transport->rx_queue[transport->rx_index].len > rx_cap) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

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
    assert((request_view.type & 0x80u) == 0u);

    ctx->last_tag = request_view.tag;
    ++ctx->call_count;

    return m9p_build_rclunk(request_view.tag, out_response_data, response_cap, out_response_len)
               ? 0
               : -(int)MESH_ERR_BAD_FRAME;
}

static int fake_mini9p_client_handler(void *client_ctx, const struct m9p_frame_view *frame)
{
    struct fake_client_ctx *ctx = (struct fake_client_ctx *)client_ctx;

    assert(ctx != NULL);
    assert(frame != NULL);

    ctx->last_tag = frame->tag;
    ctx->last_type = frame->type;
    ++ctx->call_count;
    return 0;
}

static void init_processor(
    struct mesh_processer *processor,
    struct fake_transport *transport,
    struct cluster *cluster,
    struct fake_server_ctx *server_ctx,
    struct fake_client_ctx *client_ctx,
    uint8_t local_addr)
{
    struct mesh_processer_config cfg;

    mesh_processer_get_default_config(&cfg);
    cfg.send_frame = fake_send_frame;
    cfg.receive_frame = fake_receive_frame;
    cfg.transport_ctx = transport;
    cfg.route_lookup = cluster_processor_route_lookup;
    cfg.cluster_ctx = cluster;
    cfg.control_handler = cluster_processor_control_handler;
    cfg.control_handler_ctx = NULL;
    cfg.mini9p_server_handler = fake_mini9p_server_handler;
    cfg.mini9p_server_ctx = server_ctx;
    cfg.mini9p_client_handler = fake_mini9p_client_handler;
    cfg.mini9p_client_ctx = client_ctx;
    cfg.local_addr = local_addr;
    cfg.default_hop = 8u;

    assert(mesh_processer_init(processor, &cfg) == 0);
}

static void test_forward_non_local_frame(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    uint8_t mini9p_req[96];
    size_t mini9p_req_len = 0u;
    uint8_t mesh_frame[256];
    size_t mesh_len = 0u;
    struct mesh_frame_view tx_view;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);
    assert(cluster_add_route(&cluster, 0x33u, 0x22u, 1u) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    assert(m9p_build_tclunk(0x1234u, 7u, mini9p_req, sizeof(mini9p_req), &mini9p_req_len));
    assert(mesh_build_mini9p_frame(
        0x55u,
        0x33u,
        0x0102u,
        3u,
        0u,
        mini9p_req,
        (uint16_t)mini9p_req_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));

    assert(mesh_processer_process_frame(&processor, mesh_frame, mesh_len) == 0);
    assert(transport.tx_count == 1u);
    assert(transport.tx_next_hop[0] == 0x22u);
    assert(mesh_decode_frame(transport.tx_queue[0].data, transport.tx_queue[0].len, &tx_view));
    assert(tx_view.hop == 2u);
}

static void test_local_ping_generates_pong_reply(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    struct mesh_ping_payload ping_payload;
    uint8_t ping_frame[128];
    size_t ping_len = 0u;
    struct mesh_frame_view pong_view;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);
    assert(cluster_add_route(&cluster, 0x20u, 0x21u, 1u) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    ping_payload.local_time_ms = 123456u;
    assert(mesh_build_ping(
        0x20u,
        0x10u,
        0x0203u,
        6u,
        MESH_TYPE_PING,
        &ping_payload,
        ping_frame,
        sizeof(ping_frame),
        &ping_len));

    assert(mesh_processer_process_frame(&processor, ping_frame, ping_len) == 0);
    assert(transport.tx_count == 1u);
    assert(transport.tx_next_hop[0] == 0x21u);
    assert(mesh_decode_frame(transport.tx_queue[0].data, transport.tx_queue[0].len, &pong_view));
    assert(pong_view.type == MESH_TYPE_PONG);
    assert(pong_view.src == 0x10u);
    assert(pong_view.dst == 0x20u);
}

/*
 * REGISTER helper 默认把 dst 编成 MESH_ADDR_UNASSIGNED。
 * processor 必须把这类 bootstrap REGISTER 当成本机控制帧处理，
 * 否则主机 runtime 就无法通过真实 REGISTER 自动发现新节点。
 */
static void test_register_broadcast_hits_local_control_handler(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    struct mesh_register_payload payload;
    uint8_t register_frame[128];
    size_t register_len = 0u;
    bool online = false;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    memset(&payload, 0, sizeof(payload));
    memset(payload.uid, 0x5Au, sizeof(payload.uid));
    payload.boot_nonce = 0x12345678u;
    payload.capability_bits = 0x0001u;
    payload.port_bitmap = 0x01u;

    assert(mesh_build_register(0x20u, 0x7788u, 6u, &payload, register_frame, sizeof(register_frame), &register_len));
    assert(mesh_processer_process_frame(&processor, register_frame, register_len) == 0);
    assert(cluster_get_node_online(&cluster, 0x20u, &online) == 0);
    assert(online);
    assert(transport.tx_count == 0u);
}

static void test_local_mini9p_request_to_server(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    uint8_t req_frame[96];
    size_t req_len = 0u;
    uint8_t mesh_frame[256];
    size_t mesh_len = 0u;
    struct mesh_frame_view out_view;
    struct m9p_frame_view out_m9p;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);
    assert(cluster_add_route(&cluster, 0x44u, 0x45u, 1u) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    assert(m9p_build_tclunk(0x3344u, 9u, req_frame, sizeof(req_frame), &req_len));
    assert(mesh_build_mini9p_frame(
        0x44u,
        0x10u,
        0x0101u,
        8u,
        0u,
        req_frame,
        (uint16_t)req_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));

    assert(mesh_processer_process_frame(&processor, mesh_frame, mesh_len) == 0);
    assert(server_ctx.call_count == 1u);
    assert(server_ctx.last_tag == 0x3344u);
    assert(transport.tx_count == 1u);
    assert(transport.tx_next_hop[0] == 0x45u);
    assert(mesh_decode_frame(transport.tx_queue[0].data, transport.tx_queue[0].len, &out_view));
    assert(out_view.type == MESH_TYPE_MINI9P);
    assert(m9p_decode_frame(out_view.payload, out_view.payload_len, &out_m9p));
    assert(out_m9p.tag == 0x3344u);
    assert((out_m9p.type & 0x80u) != 0u);
}

static void test_poll_once_dispatches_local_mini9p_response(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    uint8_t resp_frame[96];
    size_t resp_len = 0u;
    uint8_t mesh_frame[256];
    size_t mesh_len = 0u;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    assert(m9p_build_rclunk(0x7788u, resp_frame, sizeof(resp_frame), &resp_len));
    assert(mesh_build_mini9p_frame(
        0x66u,
        0x10u,
        0x0456u,
        7u,
        0u,
        resp_frame,
        (uint16_t)resp_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));
    fake_transport_queue_rx(&transport, mesh_frame, mesh_len);

    assert(mesh_processer_poll_once(&processor) == 0);
    assert(client_ctx.call_count == 1u);
    assert(client_ctx.last_tag == 0x7788u);
    assert((client_ctx.last_type & 0x80u) != 0u);
    assert(transport.tx_count == 0u);
}

static void test_bad_crc_frame_rejected(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    struct mesh_ping_payload ping_payload;
    uint8_t frame[128];
    size_t frame_len = 0u;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    ping_payload.local_time_ms = 321u;
    assert(mesh_build_ping(
        0x20u,
        0x10u,
        0x1234u,
        3u,
        MESH_TYPE_PING,
        &ping_payload,
        frame,
        sizeof(frame),
        &frame_len));

    frame[frame_len - 1u] ^= 0x5Au;
    assert(mesh_processer_process_frame(&processor, frame, frame_len) == -(int)MESH_ERR_BAD_FRAME);
    assert(transport.tx_count == 0u);
}

static void test_hop_exhausted_returns_no_route(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    uint8_t mini9p_req[96];
    size_t mini9p_req_len = 0u;
    uint8_t mesh_frame[256];
    size_t mesh_len = 0u;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);
    assert(cluster_add_route(&cluster, 0x33u, 0x22u, 1u) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    assert(m9p_build_tclunk(0x2233u, 7u, mini9p_req, sizeof(mini9p_req), &mini9p_req_len));
    assert(mesh_build_mini9p_frame(
        0x55u,
        0x33u,
        0x0304u,
        0u,
        0u,
        mini9p_req,
        (uint16_t)mini9p_req_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));

    assert(mesh_processer_process_frame(&processor, mesh_frame, mesh_len) == -(int)MESH_ERR_NO_ROUTE);
    assert(transport.tx_count == 0u);
}

static void test_no_route_returns_no_route(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    uint8_t mini9p_req[96];
    size_t mini9p_req_len = 0u;
    uint8_t mesh_frame[256];
    size_t mesh_len = 0u;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    assert(m9p_build_tclunk(0x4455u, 7u, mini9p_req, sizeof(mini9p_req), &mini9p_req_len));
    assert(mesh_build_mini9p_frame(
        0x55u,
        0x33u,
        0x0405u,
        4u,
        0u,
        mini9p_req,
        (uint16_t)mini9p_req_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));

    assert(mesh_processer_process_frame(&processor, mesh_frame, mesh_len) == -(int)MESH_ERR_NO_ROUTE);
    assert(transport.tx_count == 0u);
}

static void test_control_handler_null_is_tolerated(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    struct mesh_ping_payload ping_payload;
    uint8_t ping_frame[128];
    size_t ping_len = 0u;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);
    processor.config.control_handler = NULL;

    ping_payload.local_time_ms = 888u;
    assert(mesh_build_ping(
        0x20u,
        0x10u,
        0x6677u,
        4u,
        MESH_TYPE_PING,
        &ping_payload,
        ping_frame,
        sizeof(ping_frame),
        &ping_len));

    assert(mesh_processer_process_frame(&processor, ping_frame, ping_len) == 0);
    assert(transport.tx_count == 0u);
}

static void test_invalid_local_mini9p_payload_returns_bad_frame(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    uint8_t bad_payload[3] = {0x01u, 0x02u, 0x03u};
    uint8_t mesh_frame[128];
    size_t mesh_len = 0u;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    assert(mesh_build_mini9p_frame(
        0x20u,
        0x10u,
        0x7788u,
        5u,
        0u,
        bad_payload,
        (uint16_t)sizeof(bad_payload),
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));

    assert(mesh_processer_process_frame(&processor, mesh_frame, mesh_len) == -(int)MESH_ERR_BAD_FRAME);
    assert(server_ctx.call_count == 0u);
    assert(client_ctx.call_count == 0u);
    assert(transport.tx_count == 0u);
}

static void test_high_frequency_forward_burst(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    size_t i;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);
    assert(cluster_add_route(&cluster, 0x33u, 0x22u, 1u) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    for (i = 0u; i < 16u; ++i) {
        uint8_t mini9p_req[96];
        uint8_t mesh_frame[256];
        size_t mini9p_req_len = 0u;
        size_t mesh_len = 0u;

        assert(m9p_build_tclunk((uint16_t)(0x5100u + i), 7u, mini9p_req, sizeof(mini9p_req), &mini9p_req_len));
        assert(mesh_build_mini9p_frame(
            0x55u,
            0x33u,
            (uint16_t)(0x3000u + i),
            5u,
            0u,
            mini9p_req,
            (uint16_t)mini9p_req_len,
            mesh_frame,
            sizeof(mesh_frame),
            &mesh_len));
        assert(mesh_processer_process_frame(&processor, mesh_frame, mesh_len) == 0);
    }

    assert(transport.tx_count == 16u);
    for (i = 0u; i < transport.tx_count; ++i) {
        struct mesh_frame_view view;

        assert(transport.tx_next_hop[i] == 0x22u);
        assert(mesh_decode_frame(transport.tx_queue[i].data, transport.tx_queue[i].len, &view));
        assert(view.hop == 4u);
    }
}

static void test_duplicate_response_frames_processed_twice(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    uint8_t resp_frame[96];
    size_t resp_len = 0u;
    uint8_t mesh_frame[256];
    size_t mesh_len = 0u;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    assert(m9p_build_rclunk(0x7A7Bu, resp_frame, sizeof(resp_frame), &resp_len));
    assert(mesh_build_mini9p_frame(
        0x66u,
        0x10u,
        0x0556u,
        7u,
        0u,
        resp_frame,
        (uint16_t)resp_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));

    fake_transport_queue_rx(&transport, mesh_frame, mesh_len);
    fake_transport_queue_rx(&transport, mesh_frame, mesh_len);

    assert(mesh_processer_poll_once(&processor) == 0);
    assert(mesh_processer_poll_once(&processor) == 0);
    assert(client_ctx.call_count == 2u);
    assert(client_ctx.last_tag == 0x7A7Bu);
}

static void test_poll_once_mixed_queue_sequence(void)
{
    struct mesh_processer processor;
    struct fake_transport transport;
    struct fake_server_ctx server_ctx;
    struct fake_client_ctx client_ctx;
    struct cluster cluster;
    struct cluster_config cluster_cfg;
    uint8_t resp_frame[96];
    uint8_t req_frame[96];
    uint8_t req_frame2[96];
    uint8_t mesh_frame[256];
    size_t resp_len = 0u;
    size_t req_len = 0u;
    size_t req_len2 = 0u;
    size_t mesh_len = 0u;

    memset(&server_ctx, 0, sizeof(server_ctx));
    memset(&client_ctx, 0, sizeof(client_ctx));
    fake_transport_reset(&transport);

    cluster_get_default_config(&cluster_cfg);
    cluster_cfg.local_addr = 0x10u;
    cluster_cfg.mode = CLUSTER_MODE_DIRECT_TABLE;
    assert(cluster_init(&cluster, &cluster_cfg) == 0);
    assert(cluster_add_route(&cluster, 0x44u, 0x45u, 1u) == 0);
    assert(cluster_add_route(&cluster, 0x33u, 0x22u, 1u) == 0);

    init_processor(&processor, &transport, &cluster, &server_ctx, &client_ctx, 0x10u);

    /* 1) 本机 R* 响应：应进入 client 回调。 */
    assert(m9p_build_rclunk(0x1010u, resp_frame, sizeof(resp_frame), &resp_len));
    assert(mesh_build_mini9p_frame(
        0x66u,
        0x10u,
        0x0601u,
        7u,
        0u,
        resp_frame,
        (uint16_t)resp_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));
    fake_transport_queue_rx(&transport, mesh_frame, mesh_len);

    /* 2) 本机 T* 请求：应进入 server 并发送 R*。 */
    assert(m9p_build_tclunk(0x2020u, 9u, req_frame, sizeof(req_frame), &req_len));
    assert(mesh_build_mini9p_frame(
        0x44u,
        0x10u,
        0x0602u,
        7u,
        0u,
        req_frame,
        (uint16_t)req_len,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));
    fake_transport_queue_rx(&transport, mesh_frame, mesh_len);

    /* 3) 非本机目的帧：应执行转发。 */
    assert(m9p_build_tclunk(0x3030u, 9u, req_frame2, sizeof(req_frame2), &req_len2));
    assert(mesh_build_mini9p_frame(
        0x55u,
        0x33u,
        0x0603u,
        7u,
        0u,
        req_frame2,
        (uint16_t)req_len2,
        mesh_frame,
        sizeof(mesh_frame),
        &mesh_len));
    fake_transport_queue_rx(&transport, mesh_frame, mesh_len);

    /* 4) 坏 CRC：应返回 BAD_FRAME。 */
    mesh_frame[mesh_len - 1u] ^= 0xABu;
    fake_transport_queue_rx(&transport, mesh_frame, mesh_len);

    assert(mesh_processer_poll_once(&processor) == 0);
    assert(mesh_processer_poll_once(&processor) == 0);
    assert(mesh_processer_poll_once(&processor) == 0);
    assert(mesh_processer_poll_once(&processor) == -(int)MESH_ERR_BAD_FRAME);

    assert(client_ctx.call_count == 1u);
    assert(client_ctx.last_tag == 0x1010u);
    assert(server_ctx.call_count == 1u);
    assert(server_ctx.last_tag == 0x2020u);

    /* 本轮应有两次发送：一次 server 回包，一次非本机转发。 */
    assert(transport.tx_count == 2u);
}

int main(void)
{
    test_forward_non_local_frame();
    test_local_ping_generates_pong_reply();
    test_register_broadcast_hits_local_control_handler();
    test_local_mini9p_request_to_server();
    test_poll_once_dispatches_local_mini9p_response();
    test_bad_crc_frame_rejected();
    test_hop_exhausted_returns_no_route();
    test_no_route_returns_no_route();
    test_control_handler_null_is_tolerated();
    test_invalid_local_mini9p_payload_returns_bad_frame();
    test_high_frequency_forward_burst();
    test_duplicate_response_frames_processed_twice();
    test_poll_once_mixed_queue_sequence();

    puts("mesh module host tests passed (normal + abnormal + stress)");
    return 0;
}
