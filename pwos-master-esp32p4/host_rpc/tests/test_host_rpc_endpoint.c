#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "host_rpc_peer_client.h"
#include "host_rpc_service.h"
#include "pwos_host_election.h"

typedef struct {
    pwos_host_rpc_service_t service;
    pwos_host_rpc_advertise_t last_advertise;
    char write_target[16];
    char write_path[96];
    uint8_t written[32];
    uint16_t written_len;
    uint32_t topology_generation;
} test_env_t;

static int fake_read(
    void *ctx,
    const char *target,
    const char *path,
    uint8_t *data,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    static const char reply[] = "remote-ok\n";

    (void)ctx;
    assert(strcmp(target, "mcu2") == 0);
    assert(strcmp(path, "/sys/health") == 0);
    assert(deadline_ms == 500u);
    assert(*in_out_len >= sizeof(reply) - 1u);
    memcpy(data, reply, sizeof(reply) - 1u);
    *in_out_len = sizeof(reply) - 1u;
    return 0;
}

static int fake_write(
    void *ctx,
    const char *target,
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    test_env_t *env = (test_env_t *)ctx;

    assert(deadline_ms == 700u);
    snprintf(env->write_target, sizeof(env->write_target), "%s", target);
    snprintf(env->write_path, sizeof(env->write_path), "%s", path);
    memcpy(env->written, data, data_len);
    env->written_len = data_len;
    *out_written = data_len;
    return 0;
}

static int fake_advertise(
    void *ctx,
    const pwos_host_rpc_advertise_t *advertise)
{
    test_env_t *env = (test_env_t *)ctx;

    env->last_advertise = *advertise;
    return 0;
}

static int fake_local_advertise(
    void *ctx,
    pwos_host_rpc_advertise_t *out_advertise)
{
    (void)ctx;
    memset(out_advertise, 0, sizeof(*out_advertise));
    out_advertise->uid[0] = 99u;
    out_advertise->epoch = 11u;
    out_advertise->priority = 200u;
    out_advertise->role = PWOS_HOST_ROLE_LEADER;
    out_advertise->rpc_port = 9909u;
    snprintf(out_advertise->hostname, sizeof(out_advertise->hostname), "pwos-local");
    return 0;
}

static int fake_whoowns(
    void *ctx,
    const char *target,
    pwos_host_rpc_advertise_t *out_owner)
{
    (void)ctx;
    assert(strcmp(target, "mcu2") == 0);
    memset(out_owner, 0, sizeof(*out_owner));
    out_owner->uid[0] = 10u;
    out_owner->uid[1] = 20u;
    out_owner->uid[2] = 30u;
    out_owner->epoch = 7u;
    out_owner->priority = 100u;
    out_owner->role = PWOS_HOST_ROLE_LEADER;
    out_owner->rpc_port = 9909u;
    snprintf(out_owner->hostname, sizeof(out_owner->hostname), "pwos-owner");
    return 0;
}

static int fake_topology_sync(
    void *ctx,
    const pwos_host_rpc_topology_t *incoming,
    pwos_host_rpc_topology_t *out_current)
{
    test_env_t *env = (test_env_t *)ctx;

    env->topology_generation = incoming->generation;
    *out_current = *incoming;
    ++out_current->generation;
    return 0;
}

static int loopback_exchange(
    void *ctx,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_cap,
    size_t *out_response_len,
    uint32_t deadline_ms)
{
    test_env_t *env = (test_env_t *)ctx;

    assert(deadline_ms > 0u);
    return pwos_host_rpc_service_handle(
        &env->service, request, request_len,
        response, response_cap, out_response_len);
}

static void init_env(test_env_t *env, pwos_host_rpc_peer_client_t *client)
{
    pwos_host_rpc_service_config_t service_config;
    pwos_host_rpc_peer_client_config_t client_config;

    memset(env, 0, sizeof(*env));
    memset(&service_config, 0, sizeof(service_config));
    service_config.ctx = env;
    service_config.read_node = fake_read;
    service_config.write_node = fake_write;
    service_config.advertise = fake_advertise;
    service_config.local_advertise = fake_local_advertise;
    service_config.whoowns = fake_whoowns;
    service_config.topology_sync = fake_topology_sync;
    assert(pwos_host_rpc_service_init(&env->service, &service_config) == 0);

    memset(&client_config, 0, sizeof(client_config));
    client_config.io_ctx = env;
    client_config.exchange = loopback_exchange;
    assert(pwos_host_rpc_peer_client_init(client, &client_config) == 0);
}

static void test_read_write(void)
{
    test_env_t env;
    pwos_host_rpc_peer_client_t client;
    uint8_t data[64];
    uint16_t len = sizeof(data);
    uint16_t written = 0u;

    init_env(&env, &client);
    assert(pwos_host_rpc_peer_client_read_node(
        &client, "mcu2", "/sys/health", data, &len, 500u) == 0);
    assert(len == 10u && memcmp(data, "remote-ok\n", 10u) == 0);
    assert(pwos_host_rpc_peer_client_write_node(
        &client, "mcu2", "/dev/pwm",
        (const uint8_t *)"42", 2u, &written, 700u) == 0);
    assert(written == 2u);
    assert(strcmp(env.write_target, "mcu2") == 0);
    assert(strcmp(env.write_path, "/dev/pwm") == 0);
    assert(env.written_len == 2u && memcmp(env.written, "42", 2u) == 0);
}

static void test_advertise_whoowns_and_unknown(void)
{
    test_env_t env;
    pwos_host_rpc_peer_client_t client;
    pwos_host_rpc_advertise_t advertise;
    pwos_host_rpc_advertise_t owner;
    uint8_t args[256];
    uint8_t response[256];
    uint16_t args_len = 0u;
    uint16_t response_len;
    uint16_t status;

    init_env(&env, &client);
    memset(&advertise, 0, sizeof(advertise));
    advertise.uid[0] = 1u;
    advertise.uid[1] = 2u;
    advertise.uid[2] = 3u;
    advertise.epoch = 9u;
    advertise.priority = 50u;
    advertise.role = PWOS_HOST_ROLE_FOLLOWER;
    advertise.rpc_port = 9909u;
    snprintf(advertise.hostname, sizeof(advertise.hostname), "pwos-peer");
    assert(pwos_host_rpc_encode_advertise(
        &advertise, args, sizeof(args), &args_len) == 0);
    response_len = sizeof(response);
    assert(pwos_host_rpc_peer_client_call(
        &client, "host", "advertise", args, args_len, 300u,
        response, &response_len, &status) == 0);
    assert(status == PWOS_HOST_RPC_STATUS_OK);
    assert(memcmp(&env.last_advertise, &advertise, sizeof(advertise)) == 0);
    assert(pwos_host_rpc_decode_advertise(response, response_len, &owner) == 0);
    assert(strcmp(owner.hostname, "pwos-local") == 0);

    assert(pwos_host_rpc_encode_text(
        "mcu2", args, sizeof(args), &args_len) == 0);
    response_len = sizeof(response);
    assert(pwos_host_rpc_peer_client_call(
        &client, "topology", "whoowns", args, args_len, 300u,
        response, &response_len, &status) == 0);
    assert(status == PWOS_HOST_RPC_STATUS_OK);
    assert(pwos_host_rpc_decode_advertise(response, response_len, &owner) == 0);
    assert(strcmp(owner.hostname, "pwos-owner") == 0);

    response_len = sizeof(response);
    assert(pwos_host_rpc_peer_client_call(
        &client, "missing", "method", NULL, 0u, 300u,
        response, &response_len, &status) == 0);
    assert(status == PWOS_HOST_RPC_STATUS_NOT_FOUND);
}

static void test_topology_sync(void)
{
    test_env_t env;
    pwos_host_rpc_peer_client_t client;
    pwos_host_rpc_topology_t topology;
    pwos_host_rpc_topology_t response_topology;
    uint8_t args[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint8_t response[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint16_t args_len = 0u;
    uint16_t response_len = sizeof(response);
    uint16_t status;

    init_env(&env, &client);
    memset(&topology, 0, sizeof(topology));
    topology.generation = 5u;
    topology.node_count = 1u;
    snprintf(topology.nodes[0].global_target,
        sizeof(topology.nodes[0].global_target), "mcu1");
    snprintf(topology.nodes[0].owner_target,
        sizeof(topology.nodes[0].owner_target), "mcu1");
    topology.nodes[0].owner_uid[0] = 1u;
    topology.nodes[0].node_uid[0] = 100u;
    assert(pwos_host_rpc_encode_topology(
        &topology, args, sizeof(args), &args_len) == 0);
    assert(pwos_host_rpc_peer_client_call(
        &client, "topology", "sync", args, args_len, 300u,
        response, &response_len, &status) == 0);
    assert(status == PWOS_HOST_RPC_STATUS_OK);
    assert(env.topology_generation == 5u);
    assert(pwos_host_rpc_decode_topology(
        response, response_len, &response_topology) == 0);
    assert(response_topology.generation == 6u);
}

int main(void)
{
    test_read_write();
    test_advertise_whoowns_and_unknown();
    test_topology_sync();
    puts("pwos host rpc endpoint tests passed");
    return 0;
}
