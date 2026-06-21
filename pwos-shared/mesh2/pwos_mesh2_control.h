#ifndef PWOS_MESH2_CONTROL_H
#define PWOS_MESH2_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_link_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_MESH2_CONTROL_VERSION 1u

#define PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN 24u
#define PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN 28u
#define PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN 24u
#define PWOS_MESH2_LEASE_ACK_PAYLOAD_LEN 28u
#define PWOS_MESH2_LINK_STATE_PAYLOAD_LEN 24u
#define PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN 12u

#define PWOS_MESH2_NODE_CAP_RELAY 0x00000001u
#define PWOS_MESH2_NODE_CAP_COORDINATOR 0x00000002u

#define PWOS_MESH2_ASSIGN_FLAG_OK 0x01u
#define PWOS_MESH2_LINK_FLAG_UP 0x01u
#define PWOS_MESH2_LINK_FLAG_DOWN 0x02u

#define PWOS_MESH2_ROUTE_SET 1u
#define PWOS_MESH2_ROUTE_DELETE 2u

typedef struct {
    uint32_t uid[3];
    uint32_t boot_id;
    uint32_t caps;
    uint8_t upstream_port;
} pwos_mesh2_node_register_t;

typedef struct {
    uint32_t uid[3];
    uint32_t boot_id;
    uint32_t lease_epoch;
    uint32_t lease_ms;
    uint8_t addr;
    uint8_t flags;
} pwos_mesh2_addr_assign_t;

typedef struct {
    uint32_t uid[3];
    uint32_t boot_id;
    uint32_t lease_epoch;
    uint8_t addr;
} pwos_mesh2_lease_renew_t;

typedef pwos_mesh2_addr_assign_t pwos_mesh2_lease_ack_t;

typedef struct {
    uint32_t peer_uid[3];
    uint32_t peer_boot_id;
    uint16_t metric;
    uint8_t local_addr;
    uint8_t local_port;
    uint8_t peer_addr;
    uint8_t peer_port;
    uint8_t flags;
} pwos_mesh2_link_state_t;

typedef struct {
    uint32_t route_version;
    uint16_t metric;
    uint8_t dst;
    uint8_t next_hop;
    uint8_t action;
} pwos_mesh2_route_update_t;

pwos_status_t pwos_mesh2_encode_node_register(
    const pwos_mesh2_node_register_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len);

pwos_status_t pwos_mesh2_decode_node_register(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_node_register_t *out_msg);

pwos_status_t pwos_mesh2_encode_addr_assign(
    const pwos_mesh2_addr_assign_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len);

pwos_status_t pwos_mesh2_decode_addr_assign(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_addr_assign_t *out_msg);

pwos_status_t pwos_mesh2_encode_lease_renew(
    const pwos_mesh2_lease_renew_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len);

pwos_status_t pwos_mesh2_decode_lease_renew(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_lease_renew_t *out_msg);

pwos_status_t pwos_mesh2_encode_lease_ack(
    const pwos_mesh2_lease_ack_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len);

pwos_status_t pwos_mesh2_decode_lease_ack(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_lease_ack_t *out_msg);

pwos_status_t pwos_mesh2_encode_link_state(
    const pwos_mesh2_link_state_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len);

pwos_status_t pwos_mesh2_decode_link_state(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_link_state_t *out_msg);

pwos_status_t pwos_mesh2_encode_route_update(
    const pwos_mesh2_route_update_t *msg,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_len);

pwos_status_t pwos_mesh2_decode_route_update(
    const uint8_t *payload,
    size_t payload_len,
    pwos_mesh2_route_update_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_MESH2_CONTROL_H */
