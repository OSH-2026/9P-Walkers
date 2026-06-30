#ifndef PWOS_COORDINATOR_RUNTIME_H
#define PWOS_COORDINATOR_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "cluster_vfs.h"
#include "host_coordinator.h"
#include "job_manager.h"
#include "rpc_client.h"
#include "session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PWOS_COORDINATOR_UART_PORT
#define PWOS_COORDINATOR_UART_PORT 1
#endif
#ifndef PWOS_COORDINATOR_UART_TX_PIN
#define PWOS_COORDINATOR_UART_TX_PIN 37
#endif
#ifndef PWOS_COORDINATOR_UART_RX_PIN
#define PWOS_COORDINATOR_UART_RX_PIN 38
#endif
#ifndef PWOS_COORDINATOR_UART_BAUD_RATE
#define PWOS_COORDINATOR_UART_BAUD_RATE 1000000
#endif

typedef struct {
    uint8_t initialized;
    uint8_t task_started;
    uint8_t node_count;
    uint8_t uart_port;
    uint8_t control_leader;
    uint32_t rx_bytes;
    uint32_t rx_frames;
    uint32_t rx_parse_errors;
    uint32_t tx_bytes;
    uint32_t tx_frames;
    uint32_t tx_errors;
    uint32_t hello_tx;
    uint32_t hello_rx;
    uint32_t hello_ack_tx;
    uint32_t hello_ack_rx;
    uint32_t register_rx;
    uint32_t assign_tx;
    uint32_t lease_renew_rx;
    uint32_t lease_ack_tx;
    uint32_t link_state_rx;
    uint32_t route_update_tx;
    uint32_t host_advertise_tx;
    uint32_t nonleader_rx_drop;
    uint32_t data_rx;
    uint32_t mini9p_tx;
    uint32_t mini9p_rx;
    uint32_t mini9p_probe_ok;
    uint32_t mini9p_probe_fail;
    uint32_t rpc_tx;
    uint32_t rpc_rx;
    uint32_t rpc_malformed;
    uint32_t job_tx;
    uint32_t job_rx;
    uint32_t job_malformed;
    uint8_t mini9p_last_addr;
    int32_t mini9p_last_error;
    uint32_t last_rx_tick;
    uint32_t last_tx_tick;
} pwos_coordinator_runtime_stats_t;

int pwos_coordinator_runtime_start_default(void);

void pwos_coordinator_runtime_get_stats(pwos_coordinator_runtime_stats_t *out_stats);

int pwos_coordinator_runtime_get_node(
    size_t index,
    pwos_host_node_entry_t *out_node);

int pwos_coordinator_runtime_get_route(
    size_t index,
    pwos_cluster_vfs_route_t *out_route);

int pwos_coordinator_runtime_get_session(
    size_t index,
    pwos_session_snapshot_t *out_session);

void pwos_coordinator_runtime_get_session_stats(
    pwos_session_manager_stats_t *out_stats);

void pwos_coordinator_runtime_get_vfs_stats(
    pwos_cluster_vfs_stats_t *out_stats);

void pwos_coordinator_runtime_get_rpc_stats(
    pwos_rpc_client_stats_t *out_stats);

void pwos_coordinator_runtime_get_job_stats(
    pwos_job_manager_stats_t *out_stats);

int pwos_coordinator_runtime_get_job(
    size_t index,
    pwos_job_entry_t *out_job);

int pwos_coordinator_runtime_job_command(
    const char *args,
    char *output,
    size_t output_cap,
    size_t *out_len,
    uint32_t deadline_ms);

/* Lua 调度器使用的结构化 job API，避免解析 shell 文本。 */
int pwos_coordinator_runtime_job_submit(
    const char *target,
    uint8_t kernel,
    const uint8_t *input,
    uint16_t input_len,
    uint32_t deadline_ms,
    uint32_t *out_host_job_id,
    uint16_t *out_remote_status);

int pwos_coordinator_runtime_job_result(
    uint32_t host_job_id,
    uint32_t deadline_ms,
    uint8_t *out_result,
    uint16_t *in_out_len,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status);

int pwos_coordinator_runtime_rpc_call(
    const char *target,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status);

int pwos_coordinator_runtime_rpc_notify(
    const char *target,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms);

int pwos_coordinator_runtime_rpc_stream(
    const char *target,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status,
    uint16_t *out_chunk_count);

int pwos_coordinator_runtime_read_path(
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

int pwos_coordinator_runtime_write_path(
    const char *path,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms);

int pwos_coordinator_runtime_list(
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t deadline_ms);

int pwos_coordinator_runtime_stat(
    const char *path,
    struct m9p_stat *out_stat,
    uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_COORDINATOR_RUNTIME_H */
