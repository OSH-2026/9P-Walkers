#ifndef PWOS_HOST_RPC_RUNTIME_H
#define PWOS_HOST_RPC_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_host_election.h"
#include "pwos_host_rpc_methods.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_HOST_RPC_PORT 9909u
#define PWOS_HOST_RPC_MAX_PEERS PWOS_HOST_ELECTION_MAX_PEERS

typedef struct {
    uint8_t used;
    uint8_t role;
    uint32_t uid[3];
    uint32_t epoch;
    uint16_t priority;
    uint16_t port;
    uint32_t last_seen_ms;
    char hostname[PWOS_HOST_RPC_HOSTNAME_CAP];
    char ip[16];
} pwos_host_rpc_peer_snapshot_t;

typedef struct {
    uint8_t initialized;
    uint8_t server_started;
    uint8_t discovery_started;
    uint8_t local_role;
    uint8_t peer_count;
    uint8_t topology_nodes;
    uint32_t local_uid[3];
    uint32_t local_epoch;
    uint16_t local_priority;
    uint32_t leader_uid[3];
    uint32_t accepted;
    uint32_t server_requests;
    uint32_t server_errors;
    uint32_t discovery_queries;
    uint32_t discovery_results;
    uint32_t advertise_ok;
    uint32_t advertise_fail;
    uint32_t topology_sync_ok;
    uint32_t topology_sync_fail;
    uint32_t client_calls;
    uint32_t client_errors;
    uint32_t remote_reads;
    uint32_t remote_writes;
    int32_t last_error;
    char hostname[PWOS_HOST_RPC_HOSTNAME_CAP];
} pwos_host_rpc_runtime_status_t;

int pwos_host_rpc_runtime_start(void);

void pwos_host_rpc_runtime_get_status(
    pwos_host_rpc_runtime_status_t *out_status);

int pwos_host_rpc_runtime_get_peer(
    size_t index,
    pwos_host_rpc_peer_snapshot_t *out_peer);

int pwos_host_rpc_runtime_get_topology_node(
    size_t index,
    pwos_host_rpc_topology_node_t *out_node);

int pwos_host_rpc_runtime_read_path(
    const char *path,
    uint8_t *data,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

int pwos_host_rpc_runtime_write_path(
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_RUNTIME_H */
