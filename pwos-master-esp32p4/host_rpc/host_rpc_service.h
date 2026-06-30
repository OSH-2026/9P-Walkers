#ifndef PWOS_HOST_RPC_SERVICE_H
#define PWOS_HOST_RPC_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_host_rpc_methods.h"
#include "pwos_host_rpc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_HOST_RPC_SERVICE_DATA_CAP 1000u

typedef int (*pwos_host_rpc_service_read_fn)(
    void *ctx,
    const char *target,
    const char *path,
    uint8_t *data,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

typedef int (*pwos_host_rpc_service_write_fn)(
    void *ctx,
    const char *target,
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms);

typedef int (*pwos_host_rpc_service_advertise_fn)(
    void *ctx,
    const pwos_host_rpc_advertise_t *advertise);

typedef int (*pwos_host_rpc_service_local_advertise_fn)(
    void *ctx,
    pwos_host_rpc_advertise_t *out_advertise);

typedef int (*pwos_host_rpc_service_whoowns_fn)(
    void *ctx,
    const char *target,
    pwos_host_rpc_advertise_t *out_owner);

typedef int (*pwos_host_rpc_service_topology_sync_fn)(
    void *ctx,
    const pwos_host_rpc_topology_t *incoming,
    pwos_host_rpc_topology_t *out_current);

typedef struct {
    void *ctx;
    pwos_host_rpc_service_read_fn read_node;
    pwos_host_rpc_service_write_fn write_node;
    pwos_host_rpc_service_advertise_fn advertise;
    pwos_host_rpc_service_local_advertise_fn local_advertise;
    pwos_host_rpc_service_whoowns_fn whoowns;
    pwos_host_rpc_service_topology_sync_fn topology_sync;
} pwos_host_rpc_service_config_t;

typedef struct {
    uint32_t requests;
    uint32_t responses;
    uint32_t bad_frames;
    uint32_t not_found;
    uint32_t remote_errors;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t advertise_calls;
    uint32_t whoowns_calls;
    uint32_t topology_sync_calls;
    uint32_t last_call_id;
    uint16_t last_status;
} pwos_host_rpc_service_stats_t;

typedef struct {
    pwos_host_rpc_service_config_t config;
    pwos_host_rpc_service_stats_t stats;
    uint8_t data[PWOS_HOST_RPC_SERVICE_DATA_CAP];
    uint8_t method_payload[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    pwos_host_rpc_topology_t incoming_topology;
    pwos_host_rpc_topology_t outgoing_topology;
} pwos_host_rpc_service_t;

int pwos_host_rpc_service_init(
    pwos_host_rpc_service_t *service,
    const pwos_host_rpc_service_config_t *config);

int pwos_host_rpc_service_handle(
    pwos_host_rpc_service_t *service,
    const uint8_t *request_frame,
    size_t request_len,
    uint8_t *response_frame,
    size_t response_cap,
    size_t *out_response_len);

void pwos_host_rpc_service_get_stats(
    const pwos_host_rpc_service_t *service,
    pwos_host_rpc_service_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_SERVICE_H */
