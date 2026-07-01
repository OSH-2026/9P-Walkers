#include "pwos_mesh2_control.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        ++g_failures; \
        printf("CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static void test_node_register_roundtrip(void)
{
    pwos_mesh2_node_register_t in = {
        .uid = { 0x11223344u, 0x55667788u, 0x99AABBCCu },
        .boot_id = 0x12345678u,
        .caps = PWOS_MESH2_NODE_CAP_RELAY,
        .upstream_port = 3u,
    };
    pwos_mesh2_node_register_t out;
    uint8_t payload[PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN];
    size_t len = 0u;

    CHECK(pwos_mesh2_encode_node_register(&in, payload, sizeof(payload), &len) == PWOS_OK);
    CHECK(len == PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN);
    CHECK(pwos_mesh2_decode_node_register(payload, len, &out) == PWOS_OK);
    CHECK(out.uid[0] == in.uid[0]);
    CHECK(out.uid[1] == in.uid[1]);
    CHECK(out.uid[2] == in.uid[2]);
    CHECK(out.boot_id == in.boot_id);
    CHECK(out.caps == in.caps);
    CHECK(out.upstream_port == in.upstream_port);
}

static void test_assign_roundtrip(void)
{
    pwos_mesh2_addr_assign_t in = {
        .uid = { 1u, 2u, 3u },
        .boot_id = 4u,
        .lease_epoch = 5u,
        .lease_ms = 30000u,
        .addr = 0x21u,
        .flags = PWOS_MESH2_ASSIGN_FLAG_OK,
    };
    pwos_mesh2_addr_assign_t out;
    uint8_t payload[PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN];
    size_t len = 0u;

    CHECK(pwos_mesh2_encode_addr_assign(&in, payload, sizeof(payload), &len) == PWOS_OK);
    CHECK(len == PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN);
    CHECK(pwos_mesh2_decode_addr_assign(payload, len, &out) == PWOS_OK);
    CHECK(out.uid[0] == in.uid[0]);
    CHECK(out.uid[1] == in.uid[1]);
    CHECK(out.uid[2] == in.uid[2]);
    CHECK(out.boot_id == in.boot_id);
    CHECK(out.lease_epoch == in.lease_epoch);
    CHECK(out.lease_ms == in.lease_ms);
    CHECK(out.addr == in.addr);
    CHECK(out.flags == in.flags);
}

static void test_link_state_roundtrip(void)
{
    pwos_mesh2_link_state_t in = {
        .peer_uid = { 0x10u, 0x20u, 0x30u },
        .peer_boot_id = 0x40u,
        .metric = 1u,
        .local_addr = 0x11u,
        .local_port = 2u,
        .peer_addr = PWOS_LINK_ADDR_UNASSIGNED,
        .peer_port = 0u,
        .flags = PWOS_MESH2_LINK_FLAG_UP,
    };
    pwos_mesh2_link_state_t out;
    uint8_t payload[PWOS_MESH2_LINK_STATE_PAYLOAD_LEN];
    size_t len = 0u;

    CHECK(pwos_mesh2_encode_link_state(&in, payload, sizeof(payload), &len) == PWOS_OK);
    CHECK(len == PWOS_MESH2_LINK_STATE_PAYLOAD_LEN);
    CHECK(pwos_mesh2_decode_link_state(payload, len, &out) == PWOS_OK);
    CHECK(out.peer_uid[0] == in.peer_uid[0]);
    CHECK(out.peer_uid[1] == in.peer_uid[1]);
    CHECK(out.peer_uid[2] == in.peer_uid[2]);
    CHECK(out.peer_boot_id == in.peer_boot_id);
    CHECK(out.metric == in.metric);
    CHECK(out.local_addr == in.local_addr);
    CHECK(out.local_port == in.local_port);
    CHECK(out.peer_addr == in.peer_addr);
    CHECK(out.peer_port == in.peer_port);
    CHECK(out.flags == in.flags);
}

static void test_route_update_roundtrip(void)
{
    pwos_mesh2_route_update_t in = {
        .route_version = 77u,
        .metric = 3u,
        .dst = 0x22u,
        .next_hop = 0x22u,
        .action = PWOS_MESH2_ROUTE_SET,
    };
    pwos_mesh2_route_update_t out;
    uint8_t payload[PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN];
    size_t len = 0u;

    CHECK(pwos_mesh2_encode_route_update(&in, payload, sizeof(payload), &len) == PWOS_OK);
    CHECK(len == PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN);
    CHECK(pwos_mesh2_decode_route_update(payload, len, &out) == PWOS_OK);
    CHECK(out.route_version == in.route_version);
    CHECK(out.metric == in.metric);
    CHECK(out.dst == in.dst);
    CHECK(out.next_hop == in.next_hop);
    CHECK(out.action == in.action);

    payload[3] = 99u;
    CHECK(pwos_mesh2_decode_route_update(payload, len, &out) == PWOS_E_BAD_LENGTH);
}

static void test_link_frame_integration(void)
{
    pwos_mesh2_addr_assign_t assign = {
        .uid = { 9u, 8u, 7u },
        .boot_id = 6u,
        .lease_epoch = 2u,
        .lease_ms = 10000u,
        .addr = 0x31u,
        .flags = PWOS_MESH2_ASSIGN_FLAG_OK,
    };
    pwos_link_frame_view_t view;
    uint8_t payload[PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN];
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    size_t payload_len = 0u;
    size_t frame_len = 0u;

    CHECK(pwos_mesh2_encode_addr_assign(&assign, payload, sizeof(payload), &payload_len) == PWOS_OK);
    CHECK(pwos_link_encode(
        PWOS_LINK_TYPE_CTRL_ADDR_ASSIGN,
        0u,
        PWOS_LINK_ADDR_HOST,
        PWOS_LINK_ADDR_UNASSIGNED,
        8u,
        1u,
        0u,
        payload,
        (uint16_t)payload_len,
        frame,
        sizeof(frame),
        &frame_len) == PWOS_OK);
    CHECK(pwos_link_decode(frame, frame_len, &view) == PWOS_OK);
    CHECK(view.type == PWOS_LINK_TYPE_CTRL_ADDR_ASSIGN);
    CHECK(view.payload_len == PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN);
}

static void test_host_advertise_roundtrip(void)
{
    pwos_mesh2_host_advertise_t in = {
        .host_uid = {0x11223344u, 0x55667788u, 0x99AABBCCu},
        .epoch = 1234u,
        .cluster_id = 0x50574F53u,
        .priority = 200u,
        .role = PWOS_MESH2_HOST_ROLE_LEADER,
        .flags = 1u,
    };
    pwos_mesh2_host_advertise_t out;
    uint8_t payload[PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN];
    size_t len = 0u;

    CHECK(pwos_mesh2_encode_host_advertise(
        &in, payload, sizeof(payload), &len) == PWOS_OK);
    CHECK(len == PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN);
    CHECK(pwos_mesh2_decode_host_advertise(payload, len, &out) == PWOS_OK);
    CHECK(out.host_uid[0] == in.host_uid[0]);
    CHECK(out.host_uid[1] == in.host_uid[1]);
    CHECK(out.host_uid[2] == in.host_uid[2]);
    CHECK(out.epoch == in.epoch);
    CHECK(out.cluster_id == in.cluster_id);
    CHECK(out.priority == in.priority);
    CHECK(out.role == in.role);
    CHECK(out.flags == in.flags);
    payload[1] = 0xFFu;
    CHECK(pwos_mesh2_decode_host_advertise(payload, len, &out) == PWOS_E_BAD_LENGTH);
}

static void test_time_sync_roundtrip(void)
{
    pwos_mesh2_time_sync_t in = {
        .kind = PWOS_MESH2_TIME_SYNC_RESPONSE,
        .flags = PWOS_MESH2_TIME_SYNC_FLAG_WALL_VALID,
        .sequence = 0x10203040u,
        .client_tx_mono_us = UINT64_C(0x0000000123456789),
        .server_rx_unix_us = UINT64_C(1719792000123456),
        .server_tx_unix_us = UINT64_C(1719792000124567),
    };
    pwos_mesh2_time_sync_t out;
    uint8_t payload[PWOS_MESH2_TIME_SYNC_PAYLOAD_LEN];
    size_t len = 0u;

    CHECK(pwos_mesh2_encode_time_sync(
        &in, payload, sizeof(payload), &len) == PWOS_OK);
    CHECK(len == PWOS_MESH2_TIME_SYNC_PAYLOAD_LEN);
    CHECK(pwos_mesh2_decode_time_sync(payload, len, &out) == PWOS_OK);
    CHECK(out.kind == in.kind);
    CHECK(out.flags == in.flags);
    CHECK(out.sequence == in.sequence);
    CHECK(out.client_tx_mono_us == in.client_tx_mono_us);
    CHECK(out.server_rx_unix_us == in.server_rx_unix_us);
    CHECK(out.server_tx_unix_us == in.server_tx_unix_us);

    payload[1] = 0xFFu;
    CHECK(pwos_mesh2_decode_time_sync(payload, len, &out) == PWOS_E_BAD_LENGTH);
    CHECK(pwos_mesh2_encode_time_sync(
        &in, payload, sizeof(payload), &len) == PWOS_OK);
    payload[3] = 1u;
    CHECK(pwos_mesh2_decode_time_sync(payload, len, &out) == PWOS_E_BAD_LENGTH);
}

int main(void)
{
    test_node_register_roundtrip();
    test_assign_roundtrip();
    test_link_state_roundtrip();
    test_route_update_roundtrip();
    test_link_frame_integration();
    test_host_advertise_roundtrip();
    test_time_sync_roundtrip();

    if (g_failures != 0) {
        printf("pwos mesh2 control tests failed: %d\n", g_failures);
        return 1;
    }
    printf("pwos mesh2 control tests passed\n");
    return 0;
}
