#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pwos_host_rpc_protocol.h"
#include "pwos_host_election.h"
#include "pwos_host_rpc_methods.h"

static void test_round_trip(void)
{
    static const uint8_t payload[] = {0xA1u, 0x01u, 0x64u, 'm', 'c', 'u', '1'};
    uint8_t frame[PWOS_HOST_RPC_MAX_FRAME_LEN];
    pwos_host_rpc_frame_view_t view;
    size_t frame_len = 0u;

    assert(pwos_host_rpc_encode(
        PWOS_HOST_RPC_KIND_REQUEST,
        70000u,
        1500u,
        PWOS_HOST_RPC_STATUS_OK,
        "cluster",
        "read_node",
        payload,
        sizeof(payload),
        frame,
        sizeof(frame),
        &frame_len) == 0);
    assert(pwos_host_rpc_body_len(frame) == frame_len - 4u);
    assert(pwos_host_rpc_decode(frame, frame_len, &view) == 0);
    assert(view.kind == PWOS_HOST_RPC_KIND_REQUEST);
    assert(view.call_id == 70000u);
    assert(view.deadline_ms == 1500u);
    assert(view.service_len == 7u);
    assert(memcmp(view.service, "cluster", 7u) == 0);
    assert(view.method_len == 9u);
    assert(memcmp(view.method, "read_node", 9u) == 0);
    assert(view.payload_len == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);
}

static void test_limits_and_malformed(void)
{
    uint8_t frame[PWOS_HOST_RPC_MAX_FRAME_LEN];
    uint8_t payload[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    pwos_host_rpc_frame_view_t view;
    size_t frame_len = 0u;

    memset(payload, 0x5Au, sizeof(payload));
    assert(pwos_host_rpc_encode(
        PWOS_HOST_RPC_KIND_RESPONSE,
        UINT32_MAX,
        0u,
        PWOS_HOST_RPC_STATUS_OK,
        "topology",
        "sync",
        payload,
        sizeof(payload),
        frame,
        sizeof(frame),
        &frame_len) == 0);
    assert(pwos_host_rpc_decode(frame, frame_len, &view) == 0);
    assert(view.payload_len == sizeof(payload));

    assert(pwos_host_rpc_decode(frame, frame_len - 1u, &view) != 0);
    frame[3] ^= 1u;
    assert(pwos_host_rpc_decode(frame, frame_len, &view) != 0);
    frame[3] ^= 1u;
    frame[4] = 0xBFu; /* indefinite map 不允许。 */
    assert(pwos_host_rpc_decode(frame, frame_len, &view) != 0);
    assert(pwos_host_rpc_encode(
        0xFFu, 1u, 0u, 0u, "host", "advertise",
        NULL, 0u, frame, sizeof(frame), &frame_len) != 0);
}

static void test_leader_election_and_expiry(void)
{
    const uint32_t local_uid[3] = {1u, 2u, 3u};
    const uint32_t peer_a[3] = {4u, 5u, 6u};
    const uint32_t peer_b[3] = {7u, 8u, 9u};
    pwos_host_election_t election;

    assert(pwos_host_election_init(
        &election, local_uid, 10u, 100u, 1000u) == 0);
    assert(election.local_role == PWOS_HOST_ROLE_LEADER);
    assert(memcmp(election.leader.uid, local_uid, sizeof(local_uid)) == 0);

    assert(pwos_host_election_update_peer(
        &election, peer_a, 10u, 200u, 100u) == 0);
    assert(election.local_role == PWOS_HOST_ROLE_FOLLOWER);
    assert(memcmp(election.leader.uid, peer_a, sizeof(peer_a)) == 0);

    /* epoch 优先于 priority；同 epoch/priority 再由 UID 确定全序。 */
    assert(pwos_host_election_update_peer(
        &election, peer_b, 11u, 1u, 200u) == 0);
    assert(memcmp(election.leader.uid, peer_b, sizeof(peer_b)) == 0);
    assert(pwos_host_election_update_peer(
        &election, peer_a, 11u, 1u, 300u) == 0);
    assert(memcmp(election.leader.uid, peer_b, sizeof(peer_b)) == 0);

    assert(pwos_host_election_expire(&election, 1200u) == 1u);
    assert(memcmp(election.leader.uid, peer_a, sizeof(peer_a)) == 0);
    assert(pwos_host_election_expire(&election, 1300u) == 1u);
    assert(election.local_role == PWOS_HOST_ROLE_LEADER);
    assert(election.stats.leader_changes == 4u);
}

static void test_method_payloads(void)
{
    pwos_host_rpc_advertise_t advertise;
    pwos_host_rpc_advertise_t decoded;
    pwos_host_rpc_read_node_view_t read_view;
    pwos_host_rpc_write_node_view_t write_view;
    const uint8_t *blob;
    uint8_t payload[256];
    uint16_t payload_len;
    uint16_t blob_len;

    memset(&advertise, 0, sizeof(advertise));
    advertise.uid[0] = 0x11223344u;
    advertise.uid[1] = 0x55667788u;
    advertise.uid[2] = 0x99AABBCCu;
    advertise.epoch = 42u;
    advertise.priority = 200u;
    advertise.role = PWOS_HOST_ROLE_LEADER;
    advertise.rpc_port = 9909u;
    memcpy(advertise.hostname, "pwos-3344", 10u);
    assert(pwos_host_rpc_encode_advertise(
        &advertise, payload, sizeof(payload), &payload_len) == 0);
    assert(pwos_host_rpc_decode_advertise(payload, payload_len, &decoded) == 0);
    assert(memcmp(&advertise, &decoded, sizeof(advertise)) == 0);
    advertise.rpc_port = 0u;
    assert(pwos_host_rpc_encode_advertise(
        &advertise, payload, sizeof(payload), &payload_len) != 0);
    advertise.rpc_port = 9909u;
    advertise.role = 0xFFu;
    assert(pwos_host_rpc_encode_advertise(
        &advertise, payload, sizeof(payload), &payload_len) != 0);

    assert(pwos_host_rpc_encode_read_node(
        "mcu2", "/sys/health", 512u,
        payload, sizeof(payload), &payload_len) == 0);
    assert(pwos_host_rpc_decode_read_node(payload, payload_len, &read_view) == 0);
    assert(read_view.target_len == 4u);
    assert(memcmp(read_view.target, "mcu2", 4u) == 0);
    assert(read_view.path_len == 11u);
    assert(read_view.max_bytes == 512u);

    assert(pwos_host_rpc_encode_write_node(
        "mcu1", "/dev/pwm", (const uint8_t *)"42", 2u,
        payload, sizeof(payload), &payload_len) == 0);
    assert(pwos_host_rpc_decode_write_node(payload, payload_len, &write_view) == 0);
    assert(write_view.data_len == 2u);
    assert(memcmp(write_view.data, "42", 2u) == 0);

    assert(pwos_host_rpc_encode_blob(
        (const uint8_t *)"ok\n", 3u,
        payload, sizeof(payload), &payload_len) == 0);
    assert(pwos_host_rpc_decode_blob(
        payload, payload_len, &blob, &blob_len) == 0);
    assert(blob_len == 3u && memcmp(blob, "ok\n", 3u) == 0);
}

static void test_topology_payload(void)
{
    pwos_host_rpc_topology_t topology;
    pwos_host_rpc_topology_t decoded;
    uint8_t payload[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint16_t payload_len = 0u;

    memset(&topology, 0, sizeof(topology));
    topology.generation = 12u;
    topology.node_count = 2u;
    snprintf(topology.nodes[0].global_target,
        sizeof(topology.nodes[0].global_target), "mcu1");
    snprintf(topology.nodes[0].owner_target,
        sizeof(topology.nodes[0].owner_target), "mcu1");
    topology.nodes[0].owner_uid[0] = 1u;
    topology.nodes[0].node_uid[0] = 101u;
    topology.nodes[0].boot_id = 1001u;
    snprintf(topology.nodes[1].global_target,
        sizeof(topology.nodes[1].global_target), "mcu2");
    snprintf(topology.nodes[1].owner_target,
        sizeof(topology.nodes[1].owner_target), "mcu1");
    topology.nodes[1].owner_uid[0] = 2u;
    topology.nodes[1].node_uid[0] = 202u;
    topology.nodes[1].boot_id = 2002u;

    assert(pwos_host_rpc_encode_topology(
        &topology, payload, sizeof(payload), &payload_len) == 0);
    assert(pwos_host_rpc_decode_topology(payload, payload_len, &decoded) == 0);
    assert(memcmp(&topology, &decoded, sizeof(topology)) == 0);
}

int main(void)
{
    test_round_trip();
    test_limits_and_malformed();
    test_leader_election_and_expiry();
    test_method_payloads();
    test_topology_payload();
    puts("pwos host rpc protocol tests passed");
    return 0;
}
