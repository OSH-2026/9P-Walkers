#ifndef PWOS_HOST_COORDINATOR_H
#define PWOS_HOST_COORDINATOR_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_mesh2_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_HOST_COORDINATOR_MAX_NODES 32u
#define PWOS_HOST_COORDINATOR_DEFAULT_LEASE_MS 30000u

typedef struct {
    uint8_t valid;
    uint8_t addr;
    uint8_t upstream_port;
    uint32_t uid[3];
    uint32_t boot_id;
    uint32_t lease_epoch;
    uint32_t lease_ms;
} pwos_host_node_entry_t;

typedef struct {
    pwos_host_node_entry_t nodes[PWOS_HOST_COORDINATOR_MAX_NODES];
    uint8_t next_addr;
    uint32_t next_lease_epoch;
    uint32_t next_route_version;
    uint32_t register_rx;
    uint32_t duplicate_register_rx;
    uint32_t reboot_rx;
    uint32_t lease_renew_rx;
    uint32_t lease_ack_tx;
    uint32_t link_state_rx;
    uint32_t route_updates_tx;
} pwos_host_coordinator_t;

void pwos_host_coordinator_init(pwos_host_coordinator_t *coordinator);

int pwos_host_coordinator_handle_register(
    pwos_host_coordinator_t *coordinator,
    const pwos_mesh2_node_register_t *reg,
    pwos_mesh2_addr_assign_t *out_assign);

int pwos_host_coordinator_handle_lease_renew(
    pwos_host_coordinator_t *coordinator,
    const pwos_mesh2_lease_renew_t *renew,
    pwos_mesh2_lease_ack_t *out_ack);

int pwos_host_coordinator_handle_link_state(
    pwos_host_coordinator_t *coordinator,
    const pwos_mesh2_link_state_t *link,
    pwos_mesh2_route_update_t *out_route,
    uint8_t *out_route_owner_addr);

const pwos_host_node_entry_t *pwos_host_coordinator_find_by_addr(
    const pwos_host_coordinator_t *coordinator,
    uint8_t addr);

const pwos_host_node_entry_t *pwos_host_coordinator_find_by_uid(
    const pwos_host_coordinator_t *coordinator,
    const uint32_t uid[3]);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_COORDINATOR_H */
