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
    uint32_t last_time_sync_ms;
    uint32_t time_delay_us;
    int64_t time_offset_us;
    uint8_t time_valid;
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
    uint32_t time_sync_ok;
    uint32_t time_sync_fail;
    uint8_t wall_clock_valid;
    uint32_t last_time_sync_ms;
    uint32_t last_time_delay_us;
    int64_t last_time_offset_us;
    int32_t last_error;
    char hostname[PWOS_HOST_RPC_HOSTNAME_CAP];
} pwos_host_rpc_runtime_status_t;

int pwos_host_rpc_runtime_start(void);

/* 读取 Unix wall-clock。未完成 SNTP/对端校时时返回错误。 */
int pwos_host_rpc_runtime_wall_time_us(uint64_t *out_unix_us);

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

/*
 * 分布式推理：向指定主机（hostname）或本机（NULL）提交 prompt。
 * hostname 为 NULL 时走本地 dist_inference_service。
 * 返回 0 成功，其余为错误码。
 */
int pwos_host_rpc_runtime_llm_submit(
    const char *hostname,
    const char *prompt,
    uint32_t deadline_ms);

/*
 * 分布式推理：读取指定主机（hostname）或本机（NULL）的推理结果。
 * hostname 为 NULL 时走本地 dist_inference_service。
 */
int pwos_host_rpc_runtime_llm_result(
    const char *hostname,
    uint8_t *out,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

/* 读取推理状态 JSON。hostname 为 NULL 时走本地。 */
int pwos_host_rpc_runtime_llm_status(
    const char *hostname,
    uint8_t *out,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_RUNTIME_H */
