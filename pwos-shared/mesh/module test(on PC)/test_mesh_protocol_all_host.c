#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../envelope/mesh_protocal.h"
#include "../../mini9p/mini9p_protocol.h"

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static void test_crc_known_vector(void)
{
    static const uint8_t vec[] = "123456789";

    assert(mesh_crc16_ccitt_false(vec, sizeof(vec) - 1u) == 0x29B1u);
}

static void test_encode_decode_and_frame_guards(void)
{
    uint8_t payload[3] = {0x11u, 0x22u, 0x33u};
    uint8_t frame[128];
    uint8_t bad_frame[128];
    struct mesh_frame_view view;
    size_t frame_len = 0u;
    uint16_t frame_len_field;
    uint16_t crc;

    assert(mesh_encode_frame(
        MESH_TYPE_PING,
        0x01u,
        0x02u,
        0x1234u,
        5u,
        MESH_FLAG_CONTROL,
        payload,
        (uint16_t)sizeof(payload),
        frame,
        sizeof(frame),
        &frame_len));

    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.version == MESH_VERSION);
    assert(view.type == MESH_TYPE_PING);
    assert(view.src == 0x01u);
    assert(view.dst == 0x02u);
    assert(view.seq == 0x1234u);
    assert(view.hop == 5u);
    assert(view.flags == MESH_FLAG_CONTROL);
    assert(view.payload_len == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);

    memcpy(bad_frame, frame, frame_len);
    bad_frame[0] = (uint8_t)'X';
    assert(!mesh_decode_frame(bad_frame, frame_len, &view));

    assert(!mesh_decode_frame(frame, frame_len - 1u, &view));

    memcpy(bad_frame, frame, frame_len);
    bad_frame[frame_len - 1u] ^= 0xAAu;
    assert(!mesh_decode_frame(bad_frame, frame_len, &view));

    memcpy(bad_frame, frame, frame_len);
    bad_frame[4] = (uint8_t)(MESH_VERSION + 1u);
    frame_len_field = get_le16(bad_frame + 2);
    crc = mesh_crc16_ccitt_false(bad_frame + 4, frame_len_field);
    put_le16(bad_frame + frame_len - 2u, crc);
    assert(!mesh_decode_frame(bad_frame, frame_len, &view));

    assert(!mesh_encode_frame(
        MESH_TYPE_PING,
        0x01u,
        0x02u,
        0x1234u,
        5u,
        MESH_FLAG_CONTROL,
        payload,
        (uint16_t)sizeof(payload),
        frame,
        8u,
        &frame_len));
}

static void test_payload_boundaries(void)
{
    uint8_t payload[MESH_MAX_PAYLOAD_LEN];
    uint8_t frame[1024];
    struct mesh_frame_view view;
    size_t frame_len = 0u;
    size_t i;

    for (i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }

    assert(mesh_encode_frame(
        MESH_TYPE_MINI9P,
        0x10u,
        0x20u,
        0x9999u,
        7u,
        0u,
        payload,
        MESH_MAX_PAYLOAD_LEN,
        frame,
        sizeof(frame),
        &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.payload_len == MESH_MAX_PAYLOAD_LEN);
    assert(memcmp(view.payload, payload, MESH_MAX_PAYLOAD_LEN) == 0);

    assert(!mesh_encode_frame(
        MESH_TYPE_MINI9P,
        0x10u,
        0x20u,
        0x9999u,
        7u,
        0u,
        payload,
        (uint16_t)(MESH_MAX_PAYLOAD_LEN + 1u),
        frame,
        sizeof(frame),
        &frame_len));

    assert(mesh_encode_frame(
        MESH_TYPE_PING,
        0x10u,
        0x20u,
        0x7777u,
        4u,
        MESH_FLAG_CONTROL,
        NULL,
        0u,
        frame,
        sizeof(frame),
        &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.payload_len == 0u);
}

static void test_prepare_forward(void)
{
    struct mesh_frame_view view;
    uint8_t out_hop = 0u;

    memset(&view, 0, sizeof(view));
    view.hop = 4u;
    assert(mesh_prepare_forward(&view, &out_hop));
    assert(out_hop == 3u);

    view.hop = 0u;
    assert(!mesh_prepare_forward(&view, &out_hop));
}

static void test_crc_detects_single_byte_corruption(void)
{
    struct mesh_ping_payload ping;
    uint8_t frame[128];
    uint8_t bad_frame[128];
    struct mesh_frame_view view;
    size_t frame_len = 0u;
    size_t i;

    ping.local_time_ms = 0x12345678u;
    assert(mesh_build_ping(
        0x01u,
        0x02u,
        0xABCDu,
        5u,
        MESH_TYPE_PING,
        &ping,
        frame,
        sizeof(frame),
        &frame_len));

    for (i = 4u; i + 2u < frame_len; ++i) {
        memcpy(bad_frame, frame, frame_len);
        bad_frame[i] ^= 0x01u;
        assert(!mesh_decode_frame(bad_frame, frame_len, &view));
    }
}

static void test_control_type_and_error_names(void)
{
    assert(!mesh_is_control_type(MESH_TYPE_MINI9P));
    assert(mesh_is_control_type(MESH_TYPE_REGISTER));
    assert(mesh_is_control_type(MESH_TYPE_ASSIGN));
    assert(mesh_is_control_type(MESH_TYPE_PING));
    assert(mesh_is_control_type(MESH_TYPE_PONG));
    assert(mesh_is_control_type(MESH_TYPE_TIME_SYNC));
    assert(mesh_is_control_type(MESH_TYPE_ROUTE_UPDATE));
    assert(mesh_is_control_type(MESH_TYPE_LINK_STATE));
    assert(mesh_is_control_type(MESH_TYPE_ERROR));

    assert(strcmp(mesh_error_name(MESH_ERR_BAD_FRAME), "MESH_ERR_BAD_FRAME") == 0);
    assert(strcmp(mesh_error_name(MESH_ERR_UNSUPPORTED_TYPE), "MESH_ERR_UNSUPPORTED_TYPE") == 0);
    assert(strcmp(mesh_error_name(MESH_ERR_NO_ROUTE), "MESH_ERR_NO_ROUTE") == 0);
    assert(strcmp(mesh_error_name(MESH_ERR_NOT_AUTHORIZED), "MESH_ERR_NOT_AUTHORIZED") == 0);
    assert(strcmp(mesh_error_name(MESH_ERR_INVALID_STATE), "MESH_ERR_INVALID_STATE") == 0);
    assert(strcmp(mesh_error_name(MESH_ERR_BUSY), "MESH_ERR_BUSY") == 0);
    assert(strcmp(mesh_error_name(0xDEADu), "MESH_ERR_UNKNOWN") == 0);
}

static void test_mini9p_build_and_parse(void)
{
    uint8_t mini9p[96];
    size_t mini9p_len = 0u;
    uint8_t frame[256];
    size_t frame_len = 0u;
    uint8_t ping_frame[128];
    size_t ping_len = 0u;
    struct mesh_frame_view view;
    const uint8_t *payload_ptr = NULL;
    uint16_t payload_len = 0u;
    struct mesh_ping_payload ping;

    assert(m9p_build_tclunk(0x0102u, 7u, mini9p, sizeof(mini9p), &mini9p_len));
    assert(mesh_build_mini9p_frame(
        0x20u,
        0x30u,
        0x1122u,
        8u,
        (uint8_t)(MESH_FLAG_CONTROL | MESH_FLAG_NEEDS_ACK),
        mini9p,
        (uint16_t)mini9p_len,
        frame,
        sizeof(frame),
        &frame_len));

    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.type == MESH_TYPE_MINI9P);
    assert((view.flags & MESH_FLAG_CONTROL) == 0u);
    assert(mesh_parse_mini9p_payload(&view, &payload_ptr, &payload_len));
    assert(payload_len == (uint16_t)mini9p_len);
    assert(memcmp(payload_ptr, mini9p, mini9p_len) == 0);

    ping.local_time_ms = 77u;
    assert(mesh_build_ping(0x01u, 0x02u, 0x1000u, 3u, MESH_TYPE_PING, &ping, ping_frame, sizeof(ping_frame), &ping_len));
    assert(mesh_decode_frame(ping_frame, ping_len, &view));
    assert(!mesh_parse_mini9p_payload(&view, &payload_ptr, &payload_len));
}

static void test_register_roundtrip(void)
{
    struct mesh_register_payload in_payload;
    struct mesh_register_payload out_payload;
    uint8_t frame[128];
    size_t frame_len = 0u;
    struct mesh_frame_view view;

    memset(&in_payload, 0, sizeof(in_payload));
    in_payload.uid[0] = 0xA1u;
    in_payload.uid[1] = 0xA2u;
    in_payload.uid[2] = 0xA3u;
    in_payload.uid[3] = 0xA4u;
    in_payload.uid[4] = 0xA5u;
    in_payload.uid[5] = 0xA6u;
    in_payload.uid[6] = 0xA7u;
    in_payload.uid[7] = 0xA8u;
    in_payload.boot_nonce = 0x01020304u;
    in_payload.capability_bits = 0x2233u;
    in_payload.port_bitmap = 0x5Au;

    assert(mesh_build_register(0x10u, 0x2001u, 6u, &in_payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(mesh_parse_register(&view, &out_payload));
    assert(memcmp(out_payload.uid, in_payload.uid, MESH_UID_LEN) == 0);
    assert(out_payload.boot_nonce == in_payload.boot_nonce);
    assert(out_payload.capability_bits == in_payload.capability_bits);
    assert(out_payload.port_bitmap == in_payload.port_bitmap);
}

static void test_assign_roundtrip_and_invalid_name_len(void)
{
    struct mesh_assign_payload in_payload;
    struct mesh_assign_payload out_payload;
    uint8_t frame[256];
    uint8_t bad_frame[256];
    size_t frame_len = 0u;
    struct mesh_frame_view view;
    uint16_t frame_len_field;
    uint16_t crc;

    memset(&in_payload, 0, sizeof(in_payload));
    in_payload.uid[0] = 0x11u;
    in_payload.uid[1] = 0x12u;
    in_payload.uid[2] = 0x13u;
    in_payload.uid[3] = 0x14u;
    in_payload.uid[4] = 0x15u;
    in_payload.uid[5] = 0x16u;
    in_payload.uid[6] = 0x17u;
    in_payload.uid[7] = 0x18u;
    in_payload.node_addr = 0x39u;
    in_payload.lease_ms = 60000u;
    in_payload.epoch = 7u;
    strcpy(in_payload.node_name, "node-name-123456789012345678901");

    assert(strlen(in_payload.node_name) == MESH_MAX_NODE_NAME);

    assert(mesh_build_assign(0x01u, 0x39u, 0x3001u, 4u, &in_payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.type == MESH_TYPE_ASSIGN);
    assert(mesh_parse_assign(&view, &out_payload));
    assert(memcmp(out_payload.uid, in_payload.uid, MESH_UID_LEN) == 0);
    assert(out_payload.node_addr == in_payload.node_addr);
    assert(out_payload.lease_ms == in_payload.lease_ms);
    assert(out_payload.epoch == in_payload.epoch);
    assert(strcmp(out_payload.node_name, in_payload.node_name) == 0);

    memcpy(bad_frame, frame, frame_len);
    bad_frame[12u + 15u] = (uint8_t)(MESH_MAX_NODE_NAME + 1u);
    frame_len_field = get_le16(bad_frame + 2);
    crc = mesh_crc16_ccitt_false(bad_frame + 4, frame_len_field);
    put_le16(bad_frame + frame_len - 2u, crc);
    assert(mesh_decode_frame(bad_frame, frame_len, &view));
    assert(!mesh_parse_assign(&view, &out_payload));
}

static void test_assign_empty_name_roundtrip(void)
{
    struct mesh_assign_payload in_payload;
    struct mesh_assign_payload out_payload;
    uint8_t frame[256];
    size_t frame_len = 0u;
    struct mesh_frame_view view;

    memset(&in_payload, 0, sizeof(in_payload));
    in_payload.uid[0] = 0x21u;
    in_payload.uid[1] = 0x22u;
    in_payload.uid[2] = 0x23u;
    in_payload.uid[3] = 0x24u;
    in_payload.uid[4] = 0x25u;
    in_payload.uid[5] = 0x26u;
    in_payload.uid[6] = 0x27u;
    in_payload.uid[7] = 0x28u;
    in_payload.node_addr = 0x41u;
    in_payload.lease_ms = 1234u;
    in_payload.epoch = 2u;
    in_payload.node_name[0] = '\0';

    assert(mesh_build_assign(0x01u, 0x41u, 0x3002u, 4u, &in_payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(mesh_parse_assign(&view, &out_payload));
    assert(strcmp(out_payload.node_name, "") == 0);
}

static void test_register_header_semantics(void)
{
    struct mesh_register_payload payload;
    uint8_t frame[128];
    size_t frame_len = 0u;
    struct mesh_frame_view view;

    memset(&payload, 0, sizeof(payload));
    payload.uid[0] = 0x11u;

    assert(mesh_build_register(0x66u, 0x9001u, 6u, &payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.type == MESH_TYPE_REGISTER);
    assert(view.dst == MESH_ADDR_UNASSIGNED);
    assert((view.flags & MESH_FLAG_CONTROL) != 0u);
    assert((view.flags & MESH_FLAG_NEEDS_ACK) != 0u);
}

static void test_ping_and_pong_roundtrip(void)
{
    struct mesh_ping_payload in_payload;
    struct mesh_ping_payload out_payload;
    uint8_t frame[128];
    size_t frame_len = 0u;
    struct mesh_frame_view view;

    in_payload.local_time_ms = 0x0A0B0C0Du;

    assert(mesh_build_ping(0x10u, 0x20u, 0x3344u, 3u, MESH_TYPE_PING, &in_payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.type == MESH_TYPE_PING);
    assert(mesh_parse_ping(&view, &out_payload));
    assert(out_payload.local_time_ms == in_payload.local_time_ms);

    assert(mesh_build_ping(0x20u, 0x10u, 0x3345u, 3u, MESH_TYPE_PONG, &in_payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(view.type == MESH_TYPE_PONG);
    assert(mesh_parse_ping(&view, &out_payload));
    assert(out_payload.local_time_ms == in_payload.local_time_ms);

    assert(!mesh_build_ping(0x20u, 0x10u, 0x3345u, 3u, MESH_TYPE_ASSIGN, &in_payload, frame, sizeof(frame), &frame_len));
}

static void test_time_sync_roundtrip(void)
{
    struct mesh_time_sync_payload in_payload;
    struct mesh_time_sync_payload out_payload;
    uint8_t frame[128];
    size_t frame_len = 0u;
    struct mesh_frame_view view;

    in_payload.t0_master_send = 100u;
    in_payload.t1_slave_recv = 110u;
    in_payload.t2_slave_send = 120u;
    in_payload.t3_master_recv = 130u;

    assert(mesh_build_time_sync(0x01u, 0x02u, 0x0101u, 5u, &in_payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(mesh_parse_time_sync(&view, &out_payload));
    assert(out_payload.t0_master_send == in_payload.t0_master_send);
    assert(out_payload.t1_slave_recv == in_payload.t1_slave_recv);
    assert(out_payload.t2_slave_send == in_payload.t2_slave_send);
    assert(out_payload.t3_master_recv == in_payload.t3_master_recv);
}

static void test_route_update_roundtrip_and_invalid_action(void)
{
    struct mesh_route_update_payload in_set;
    struct mesh_route_update_payload in_del;
    struct mesh_route_update_payload out_payload;
    uint8_t frame[128];
    uint8_t bad_frame[128];
    size_t frame_len = 0u;
    struct mesh_frame_view view;
    uint16_t frame_len_field;
    uint16_t crc;

    in_set.dst = 0x40u;
    in_set.next_hop = 0x41u;
    in_set.metric = 2u;
    in_set.route_version = 10u;
    in_set.action = MESH_ROUTE_SET;

    in_del.dst = 0x40u;
    in_del.next_hop = 0x41u;
    in_del.metric = 2u;
    in_del.route_version = 11u;
    in_del.action = MESH_ROUTE_DELETE;

    assert(mesh_build_route_update(0x01u, 0x10u, 0x1111u, 4u, &in_set, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(mesh_parse_route_update(&view, &out_payload));
    assert(out_payload.dst == in_set.dst);
    assert(out_payload.next_hop == in_set.next_hop);
    assert(out_payload.metric == in_set.metric);
    assert(out_payload.route_version == in_set.route_version);
    assert(out_payload.action == in_set.action);

    assert(mesh_build_route_update(0x01u, 0x10u, 0x1112u, 4u, &in_del, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(mesh_parse_route_update(&view, &out_payload));
    assert(out_payload.action == MESH_ROUTE_DELETE);

    in_set.action = 0x99u;
    assert(!mesh_build_route_update(0x01u, 0x10u, 0x1111u, 4u, &in_set, frame, sizeof(frame), &frame_len));

    assert(mesh_build_route_update(0x01u, 0x10u, 0x1113u, 4u, &in_del, frame, sizeof(frame), &frame_len));
    memcpy(bad_frame, frame, frame_len);
    bad_frame[12u + 5u] = 0x7Fu;
    frame_len_field = get_le16(bad_frame + 2);
    crc = mesh_crc16_ccitt_false(bad_frame + 4, frame_len_field);
    put_le16(bad_frame + frame_len - 2u, crc);
    assert(mesh_decode_frame(bad_frame, frame_len, &view));
    assert(!mesh_parse_route_update(&view, &out_payload));
}

static void test_link_state_roundtrip(void)
{
    struct mesh_link_state_payload in_payload;
    struct mesh_link_state_payload out_payload;
    uint8_t frame[128];
    size_t frame_len = 0u;
    struct mesh_frame_view view;

    in_payload.neighbor = 0x55u;
    in_payload.link_up = 1u;
    in_payload.quality = 80u;
    in_payload.local_port = 2u;

    assert(mesh_build_link_state(0x11u, 0x22u, 0x0909u, 3u, &in_payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(mesh_parse_link_state(&view, &out_payload));
    assert(out_payload.neighbor == in_payload.neighbor);
    assert(out_payload.link_up == in_payload.link_up);
    assert(out_payload.quality == in_payload.quality);
    assert(out_payload.local_port == in_payload.local_port);
}

static void test_error_roundtrip(void)
{
    struct mesh_error_payload in_payload;
    struct mesh_error_payload out_payload;
    uint8_t frame[128];
    size_t frame_len = 0u;
    struct mesh_frame_view view;

    in_payload.code = MESH_ERR_NO_ROUTE;
    in_payload.related_seq = 0x8888u;

    assert(mesh_build_error(0x99u, 0x01u, 0x0202u, 3u, &in_payload, frame, sizeof(frame), &frame_len));
    assert(mesh_decode_frame(frame, frame_len, &view));
    assert(mesh_parse_error(&view, &out_payload));
    assert(out_payload.code == in_payload.code);
    assert(out_payload.related_seq == in_payload.related_seq);
}

int main(void)
{
    test_crc_known_vector();
    test_encode_decode_and_frame_guards();
    test_payload_boundaries();
    test_prepare_forward();
    test_crc_detects_single_byte_corruption();
    test_control_type_and_error_names();
    test_mini9p_build_and_parse();
    test_register_roundtrip();
    test_register_header_semantics();
    test_assign_roundtrip_and_invalid_name_len();
    test_assign_empty_name_roundtrip();
    test_ping_and_pong_roundtrip();
    test_time_sync_roundtrip();
    test_route_update_roundtrip_and_invalid_action();
    test_link_state_roundtrip();
    test_error_roundtrip();

    puts("mesh protocol all tests passed");
    return 0;
}
