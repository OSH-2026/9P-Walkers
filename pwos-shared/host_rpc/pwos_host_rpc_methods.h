#ifndef PWOS_HOST_RPC_METHODS_H
#define PWOS_HOST_RPC_METHODS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_HOST_RPC_TARGET_CAP 16u
#define PWOS_HOST_RPC_PATH_CAP 96u
#define PWOS_HOST_RPC_HOSTNAME_CAP 32u
#define PWOS_HOST_RPC_TOPOLOGY_MAX_NODES 12u
#define PWOS_HOST_RPC_TIME_VERSION 1u
#define PWOS_HOST_RPC_TIME_FLAG_WALL_VALID 0x01u
#define PWOS_HOST_RPC_TIME_EXCHANGE_PAYLOAD_LEN 32u

typedef struct {
    uint32_t uid[3];
    uint32_t epoch;
    uint16_t priority;
    uint8_t role;
    uint16_t rpc_port;
    char hostname[PWOS_HOST_RPC_HOSTNAME_CAP];
} pwos_host_rpc_advertise_t;

typedef struct {
    const char *target;
    uint8_t target_len;
    const char *path;
    uint8_t path_len;
    uint16_t max_bytes;
} pwos_host_rpc_read_node_view_t;

typedef struct {
    const char *target;
    uint8_t target_len;
    const char *path;
    uint8_t path_len;
    const uint8_t *data;
    uint16_t data_len;
} pwos_host_rpc_write_node_view_t;

typedef struct {
    char global_target[PWOS_HOST_RPC_TARGET_CAP];
    char owner_target[PWOS_HOST_RPC_TARGET_CAP];
    uint32_t owner_uid[3];
    uint32_t node_uid[3];
    uint32_t boot_id;
} pwos_host_rpc_topology_node_t;

typedef struct {
    uint32_t generation;
    uint8_t node_count;
    pwos_host_rpc_topology_node_t nodes[PWOS_HOST_RPC_TOPOLOGY_MAX_NODES];
} pwos_host_rpc_topology_t;

typedef struct {
    uint8_t flags;
    uint32_t sequence;
    uint64_t client_tx_mono_us;
    uint64_t server_rx_unix_us;
    uint64_t server_tx_unix_us;
} pwos_host_rpc_time_exchange_t;

int pwos_host_rpc_encode_advertise(
    const pwos_host_rpc_advertise_t *advertise,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len);

int pwos_host_rpc_decode_advertise(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_advertise_t *out_advertise);

int pwos_host_rpc_encode_read_node(
    const char *target,
    const char *path,
    uint16_t max_bytes,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len);

int pwos_host_rpc_decode_read_node(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_read_node_view_t *out_view);

int pwos_host_rpc_encode_write_node(
    const char *target,
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len);

int pwos_host_rpc_decode_write_node(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_write_node_view_t *out_view);

/* 结果 payload 也保持为嵌套 CBOR bytes，而不是裸结构体。 */
int pwos_host_rpc_encode_blob(
    const uint8_t *data,
    uint16_t data_len,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len);

int pwos_host_rpc_decode_blob(
    const uint8_t *payload,
    uint16_t payload_len,
    const uint8_t **out_data,
    uint16_t *out_data_len);

int pwos_host_rpc_encode_text(
    const char *text,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len);

int pwos_host_rpc_decode_text(
    const uint8_t *payload,
    uint16_t payload_len,
    const char **out_text,
    uint8_t *out_text_len);

int pwos_host_rpc_encode_topology(
    const pwos_host_rpc_topology_t *topology,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len);

int pwos_host_rpc_decode_topology(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_topology_t *out_topology);

int pwos_host_rpc_encode_time_exchange(
    const pwos_host_rpc_time_exchange_t *exchange,
    uint8_t *out,
    size_t out_cap,
    uint16_t *out_len);

int pwos_host_rpc_decode_time_exchange(
    const uint8_t *payload,
    uint16_t payload_len,
    pwos_host_rpc_time_exchange_t *out_exchange);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_METHODS_H */
