#include "host_coordinator.h"

#include <string.h>

#include "pwos_link_frame.h"

static uint8_t uid_equal(const uint32_t a[3], const uint32_t b[3])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static pwos_host_node_entry_t *find_mut_by_uid(
    pwos_host_coordinator_t *coordinator,
    const uint32_t uid[3])
{
    size_t i;

    if (coordinator == NULL || uid == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        if (coordinator->nodes[i].valid != 0u &&
            uid_equal(coordinator->nodes[i].uid, uid)) {
            return &coordinator->nodes[i];
        }
    }
    return NULL;
}

static uint8_t addr_in_use(const pwos_host_coordinator_t *coordinator, uint8_t addr)
{
    return pwos_host_coordinator_find_by_addr(coordinator, addr) != NULL ? 1u : 0u;
}

static int alloc_addr(pwos_host_coordinator_t *coordinator, uint8_t *out_addr)
{
    uint16_t tries;
    uint8_t candidate;

    if (coordinator == NULL || out_addr == NULL) {
        return -1;
    }

    candidate = coordinator->next_addr;
    if (candidate == PWOS_LINK_ADDR_HOST || candidate == PWOS_LINK_ADDR_UNASSIGNED) {
        candidate = 1u;
    }

    for (tries = 0u; tries < 254u; ++tries) {
        if (candidate == PWOS_LINK_ADDR_HOST || candidate == PWOS_LINK_ADDR_UNASSIGNED) {
            candidate = 1u;
        }
        if (addr_in_use(coordinator, candidate) == 0u) {
            *out_addr = candidate;
            coordinator->next_addr = (uint8_t)(candidate + 1u);
            if (coordinator->next_addr == PWOS_LINK_ADDR_UNASSIGNED) {
                coordinator->next_addr = 1u;
            }
            return 0;
        }
        ++candidate;
    }
    return -1;
}

static pwos_host_node_entry_t *alloc_node(pwos_host_coordinator_t *coordinator)
{
    size_t i;

    if (coordinator == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        if (coordinator->nodes[i].valid == 0u) {
            memset(&coordinator->nodes[i], 0, sizeof(coordinator->nodes[i]));
            coordinator->nodes[i].valid = 1u;
            return &coordinator->nodes[i];
        }
    }
    return NULL;
}

static void fill_assign(
    pwos_host_coordinator_t *coordinator,
    const pwos_host_node_entry_t *node,
    pwos_mesh2_addr_assign_t *out_assign)
{
    memset(out_assign, 0, sizeof(*out_assign));
    out_assign->uid[0] = node->uid[0];
    out_assign->uid[1] = node->uid[1];
    out_assign->uid[2] = node->uid[2];
    out_assign->boot_id = node->boot_id;
    out_assign->lease_epoch = node->lease_epoch;
    out_assign->lease_ms = node->lease_ms;
    out_assign->addr = node->addr;
    out_assign->flags = PWOS_MESH2_ASSIGN_FLAG_OK;
    (void)coordinator;
}

void pwos_host_coordinator_init(pwos_host_coordinator_t *coordinator)
{
    if (coordinator == NULL) {
        return;
    }

    memset(coordinator, 0, sizeof(*coordinator));
    coordinator->next_addr = 1u;
    coordinator->next_lease_epoch = 1u;
    coordinator->next_route_version = 1u;
}

int pwos_host_coordinator_handle_register(
    pwos_host_coordinator_t *coordinator,
    const pwos_mesh2_node_register_t *reg,
    pwos_mesh2_addr_assign_t *out_assign)
{
    pwos_host_node_entry_t *node;
    uint8_t addr;

    if (coordinator == NULL || reg == NULL || out_assign == NULL) {
        return -1;
    }

    ++coordinator->register_rx;
    node = find_mut_by_uid(coordinator, reg->uid);
    if (node == NULL) {
        node = alloc_node(coordinator);
        if (node == NULL || alloc_addr(coordinator, &addr) != 0) {
            return -1;
        }
        node->addr = addr;
        node->uid[0] = reg->uid[0];
        node->uid[1] = reg->uid[1];
        node->uid[2] = reg->uid[2];
        node->boot_id = reg->boot_id;
        node->lease_epoch = coordinator->next_lease_epoch++;
        node->lease_ms = PWOS_HOST_COORDINATOR_DEFAULT_LEASE_MS;
    } else if (node->boot_id != reg->boot_id) {
        node->boot_id = reg->boot_id;
        node->lease_epoch = coordinator->next_lease_epoch++;
        ++coordinator->reboot_rx;
    } else {
        ++coordinator->duplicate_register_rx;
    }

    node->upstream_port = reg->upstream_port;
    fill_assign(coordinator, node, out_assign);
    return 0;
}

int pwos_host_coordinator_handle_lease_renew(
    pwos_host_coordinator_t *coordinator,
    const pwos_mesh2_lease_renew_t *renew,
    pwos_mesh2_lease_ack_t *out_ack)
{
    pwos_host_node_entry_t *node;

    if (coordinator == NULL || renew == NULL || out_ack == NULL) {
        return -1;
    }

    ++coordinator->lease_renew_rx;
    node = find_mut_by_uid(coordinator, renew->uid);
    if (node == NULL || node->boot_id != renew->boot_id) {
        return -1;
    }

    /*
     * 租约续期只确认 host 当前认为有效的地址和 epoch。
     * 如果节点带着旧 addr/epoch 续期，ACK 会把它重新拉回 host 的真相。
     */
    fill_assign(coordinator, node, out_ack);
    ++coordinator->lease_ack_tx;
    return 0;
}

int pwos_host_coordinator_handle_link_state(
    pwos_host_coordinator_t *coordinator,
    const pwos_mesh2_link_state_t *link,
    pwos_mesh2_route_update_t *out_route,
    uint8_t *out_route_owner_addr)
{
    const pwos_host_node_entry_t *local;
    const pwos_host_node_entry_t *peer;

    if (coordinator == NULL || link == NULL ||
        out_route == NULL || out_route_owner_addr == NULL) {
        return -1;
    }

    ++coordinator->link_state_rx;
    local = pwos_host_coordinator_find_by_addr(coordinator, link->local_addr);
    peer = pwos_host_coordinator_find_by_uid(coordinator, link->peer_uid);
    if (local == NULL || peer == NULL) {
        return 0;
    }

    memset(out_route, 0, sizeof(*out_route));
    out_route->dst = peer->addr;
    out_route->next_hop = peer->addr;
    out_route->metric = (uint16_t)(link->metric + 1u);
    out_route->route_version = coordinator->next_route_version++;
    out_route->action = (link->flags & PWOS_MESH2_LINK_FLAG_UP) != 0u ?
        PWOS_MESH2_ROUTE_SET : PWOS_MESH2_ROUTE_DELETE;
    *out_route_owner_addr = local->addr;
    ++coordinator->route_updates_tx;
    return 1;
}

const pwos_host_node_entry_t *pwos_host_coordinator_find_by_addr(
    const pwos_host_coordinator_t *coordinator,
    uint8_t addr)
{
    size_t i;

    if (coordinator == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        if (coordinator->nodes[i].valid != 0u &&
            coordinator->nodes[i].addr == addr) {
            return &coordinator->nodes[i];
        }
    }
    return NULL;
}

const pwos_host_node_entry_t *pwos_host_coordinator_find_by_uid(
    const pwos_host_coordinator_t *coordinator,
    const uint32_t uid[3])
{
    size_t i;

    if (coordinator == NULL || uid == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        if (coordinator->nodes[i].valid != 0u &&
            uid_equal(coordinator->nodes[i].uid, uid)) {
            return &coordinator->nodes[i];
        }
    }
    return NULL;
}
