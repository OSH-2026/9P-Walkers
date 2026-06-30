#include "node_control.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"

#include "port_manager.h"
#include "pwos_link_frame.h"
#include "pwos_mesh2_control.h"
#include "pwos_queues.h"
#include "uart_dma_port.h"

#define PWOS_NODE_REGISTER_INTERVAL_MS 1000u
#define PWOS_NODE_LINK_STATE_INTERVAL_MS 2000u
#define PWOS_NODE_DEFAULT_LEASE_MS 10000u
#define PWOS_NODE_DEFAULT_TTL 8u
#define PWOS_NODE_DIRECT_ROUTE_VERSION 0u
#define PWOS_NODE_HOST_ADVERTISE_TIMEOUT_MS 5000u
#define PWOS_NODE_CLUSTER_ID 0x50574F53u
#define PWOS_NODE_HOST_ADVERTISE_CACHE_SIZE 8u

typedef struct {
    uint8_t valid;
    uint8_t dst;
    uint8_t next_hop;
    uint8_t port_id;
    uint16_t metric;
    uint32_t route_version;
} pwos_node_route_t;

typedef struct {
    uint8_t valid;
    uint8_t port_id;
    uint32_t uid[3];
    uint32_t boot_id;
    uint32_t last_tick;
} pwos_pending_register_t;

typedef struct {
    uint8_t valid;
    uint16_t seq;
    uint32_t host_uid[3];
    uint32_t epoch;
    uint32_t last_tick;
} pwos_host_advertise_seen_t;

static pwos_node_state_t g_state;
static uint8_t g_local_addr;
static uint8_t g_upstream_port;
static uint32_t g_local_uid[3];
static uint32_t g_local_boot_id;
static uint32_t g_lease_epoch;
static uint32_t g_lease_ms;
static uint32_t g_last_register_tick;
static uint32_t g_last_renew_tick;
static uint32_t g_last_link_state_tick;
static uint16_t g_next_seq;
static pwos_node_route_t g_routes[PWOS_NODE_CONTROL_MAX_ROUTES];
static pwos_pending_register_t g_pending[PWOS_NODE_CONTROL_MAX_PENDING];
static pwos_node_control_snapshot_t g_stats;
static uint8_t g_authority_valid;
static uint8_t g_authority_port;
static uint32_t g_authority_uid[3];
static uint32_t g_authority_epoch;
static uint16_t g_authority_priority;
static uint8_t g_authority_ttl;
static uint32_t g_authority_last_tick;
static pwos_host_advertise_seen_t
    g_host_advertise_seen[PWOS_NODE_HOST_ADVERTISE_CACHE_SIZE];

static uint8_t uid_equal(const uint32_t a[3], const uint32_t b[3])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static int host_candidate_compare(
    uint32_t epoch,
    uint16_t priority,
    const uint32_t uid[3])
{
    size_t i;

    if (priority != g_authority_priority) return priority > g_authority_priority ? 1 : -1;
    if (epoch != g_authority_epoch) return epoch > g_authority_epoch ? 1 : -1;
    for (i = 0u; i < 3u; ++i) {
        if (uid[i] != g_authority_uid[i]) return uid[i] > g_authority_uid[i] ? 1 : -1;
    }
    return 0;
}

static int control_from_authority(uint8_t ingress_port)
{
    if (g_authority_valid == 0u || ingress_port == g_authority_port) {
        return 1;
    }
    ++g_stats.nonleader_ctrl_drop;
    return 0;
}

static int select_control_upstream(uint8_t *out_port)
{
    if (out_port == NULL) {
        return -1;
    }
    if (g_authority_valid != 0u) {
        *out_port = g_authority_port;
        return 0;
    }
    return pwos_port_manager_select_upstream(out_port);
}

static uint8_t is_local_identity(const uint32_t uid[3], uint32_t boot_id)
{
    return uid_equal(uid, g_local_uid) && boot_id == g_local_boot_id;
}

static void clear_routes(void)
{
    memset(g_routes, 0, sizeof(g_routes));
}

static pwos_node_route_t *find_route(uint8_t dst)
{
    size_t i;

    for (i = 0u; i < PWOS_NODE_CONTROL_MAX_ROUTES; ++i) {
        if (g_routes[i].valid != 0u && g_routes[i].dst == dst) {
            return &g_routes[i];
        }
    }
    return NULL;
}

static pwos_node_route_t *alloc_route(uint8_t dst)
{
    size_t i;
    pwos_node_route_t *route;

    route = find_route(dst);
    if (route != NULL) {
        return route;
    }

    for (i = 0u; i < PWOS_NODE_CONTROL_MAX_ROUTES; ++i) {
        if (g_routes[i].valid == 0u) {
            memset(&g_routes[i], 0, sizeof(g_routes[i]));
            g_routes[i].valid = 1u;
            g_routes[i].dst = dst;
            return &g_routes[i];
        }
    }
    return NULL;
}

static void set_direct_route(uint8_t dst, uint8_t port_id)
{
    pwos_node_route_t *route;

    if (dst == PWOS_LINK_ADDR_UNASSIGNED ||
        dst == PWOS_LINK_ADDR_HOST ||
        dst == g_local_addr) {
        return;
    }

    taskENTER_CRITICAL();
    route = alloc_route(dst);
    if (route != NULL) {
        route->next_hop = dst;
        route->port_id = port_id;
        route->metric = 1u;
        route->route_version = PWOS_NODE_DIRECT_ROUTE_VERSION;
    }
    taskEXIT_CRITICAL();
    /* 同步更新端口的 peer_addr，使 resolve_next_hop_port 可以按 addr 查端口。 */
    pwos_port_manager_set_peer_addr(port_id, dst);
}

static int resolve_next_hop_port(uint8_t next_hop, uint8_t *out_port_id)
{
    pwos_node_route_t *route;

    if (out_port_id == NULL) {
        return -1;
    }
    if (next_hop == PWOS_LINK_ADDR_HOST) {
        if (g_upstream_port == PWOS_LINK_ADDR_UNASSIGNED) {
            return -1;
        }
        *out_port_id = g_upstream_port;
        return 0;
    }

    route = find_route(next_hop);
    if (route != NULL) {
        *out_port_id = route->port_id;
        return 0;
    }

    /* Fallback: search ports by peer_addr (learned from HELLO/forwarded frames). */
    {
        int port_id = pwos_port_manager_find_port_by_peer_addr(next_hop);
        if (port_id >= 0) {
            *out_port_id = (uint8_t)port_id;
            return 0;
        }
    }
    return -1;
}

static void record_pending_register(
    const pwos_mesh2_node_register_t *msg,
    uint8_t ingress_port)
{
    size_t i;
    size_t free_index = PWOS_NODE_CONTROL_MAX_PENDING;

    if (msg == NULL) {
        return;
    }

    for (i = 0u; i < PWOS_NODE_CONTROL_MAX_PENDING; ++i) {
        if (g_pending[i].valid != 0u &&
            uid_equal(g_pending[i].uid, msg->uid) &&
            g_pending[i].boot_id == msg->boot_id) {
            g_pending[i].port_id = ingress_port;
            g_pending[i].last_tick = (uint32_t)xTaskGetTickCount();
            return;
        }
        if (g_pending[i].valid == 0u && free_index == PWOS_NODE_CONTROL_MAX_PENDING) {
            free_index = i;
        }
    }

    if (free_index < PWOS_NODE_CONTROL_MAX_PENDING) {
        g_pending[free_index].valid = 1u;
        g_pending[free_index].port_id = ingress_port;
        g_pending[free_index].boot_id = msg->boot_id;
        g_pending[free_index].uid[0] = msg->uid[0];
        g_pending[free_index].uid[1] = msg->uid[1];
        g_pending[free_index].uid[2] = msg->uid[2];
        g_pending[free_index].last_tick = (uint32_t)xTaskGetTickCount();
    }
}

static int take_pending_assign(
    const pwos_mesh2_addr_assign_t *assign,
    uint8_t *out_port_id)
{
    size_t i;

    if (assign == NULL || out_port_id == NULL) {
        return -1;
    }

    for (i = 0u; i < PWOS_NODE_CONTROL_MAX_PENDING; ++i) {
        if (g_pending[i].valid != 0u &&
            uid_equal(g_pending[i].uid, assign->uid) &&
            g_pending[i].boot_id == assign->boot_id) {
            *out_port_id = g_pending[i].port_id;
            g_pending[i].valid = 0u;
            return 0;
        }
    }
    return -1;
}

static int send_encoded_control(
    uint8_t type,
    uint8_t dst,
    uint8_t port_id,
    const uint8_t *payload,
    uint16_t payload_len)
{
    pwos_frame_block_t *block;
    size_t frame_len = 0u;
    uint8_t src;
    pwos_status_t status;

    block = pwos_frame_pool_alloc();
    if (block == NULL) {
        return -1;
    }

    src = g_state == PWOS_NODE_ASSIGNED ? g_local_addr : PWOS_LINK_ADDR_UNASSIGNED;
    status = pwos_link_encode(
        type,
        0u,
        src,
        dst,
        PWOS_NODE_DEFAULT_TTL,
        ++g_next_seq,
        0u,
        payload,
        payload_len,
        block->data,
        sizeof(block->data),
        &frame_len);
    if (status != PWOS_OK) {
        pwos_frame_pool_free(block);
        return -1;
    }

    block->port_id = port_id;
    block->len = (uint16_t)frame_len;
    block->timestamp_ms = (uint32_t)xTaskGetTickCount();
    if (pwos_ctrl_tx_send(block, 0u) != pdPASS) {
        pwos_frame_pool_free(block);
        return -1;
    }

    return 0;
}

static int send_encoded_data(
    uint8_t type,
    uint8_t dst,
    uint8_t port_id,
    const uint8_t *payload,
    uint16_t payload_len)
{
    pwos_frame_block_t *block;
    size_t frame_len = 0u;
    pwos_status_t status;

    if (!pwos_link_type_is_data(type) || g_state != PWOS_NODE_ASSIGNED) {
        return -1;
    }

    block = pwos_frame_pool_alloc();
    if (block == NULL) {
        return -1;
    }

    status = pwos_link_encode(
        type,
        0u,
        g_local_addr,
        dst,
        PWOS_NODE_DEFAULT_TTL,
        ++g_next_seq,
        0u,
        payload,
        payload_len,
        block->data,
        sizeof(block->data),
        &frame_len);
    if (status != PWOS_OK) {
        pwos_frame_pool_free(block);
        return -1;
    }

    block->port_id = port_id;
    block->len = (uint16_t)frame_len;
    block->timestamp_ms = (uint32_t)xTaskGetTickCount();
    if (pwos_link_tx_send(block, 0u) != pdPASS) {
        pwos_frame_pool_free(block);
        return -1;
    }

    return 0;
}

static int forward_view(
    const pwos_link_frame_view_t *view,
    uint8_t port_id)
{
    pwos_frame_block_t *block;
    size_t frame_len = 0u;
    pwos_status_t status;

    if (view == NULL || view->ttl <= 1u) {
        ++g_stats.drop_no_route;
        return -1;
    }

    block = pwos_frame_pool_alloc();
    if (block == NULL) {
        ++g_stats.drop_no_route;
        return -1;
    }

    status = pwos_link_encode(
        view->type,
        view->flags,
        view->src,
        view->dst,
        (uint8_t)(view->ttl - 1u),
        view->seq,
        view->ack,
        view->payload,
        view->payload_len,
        block->data,
        sizeof(block->data),
        &frame_len);
    if (status != PWOS_OK) {
        pwos_frame_pool_free(block);
        ++g_stats.bad_ctrl_frames;
        return -1;
    }

    block->port_id = port_id;
    block->len = (uint16_t)frame_len;
    block->timestamp_ms = (uint32_t)xTaskGetTickCount();

    if (pwos_link_type_is_control(view->type)) {
        if (pwos_ctrl_tx_send(block, 0u) != pdPASS) {
            pwos_frame_pool_free(block);
            ++g_stats.drop_no_route;
            return -1;
        }
    } else {
        if (pwos_link_tx_send(block, 0u) != pdPASS) {
            pwos_frame_pool_free(block);
            ++g_stats.drop_no_route;
            return -1;
        }
    }

    ++g_stats.forwarded_ctrl;
    return 0;
}

static int remember_host_advertise(
    const pwos_mesh2_host_advertise_t *advertise,
    uint16_t seq)
{
    size_t i;
    size_t replace_index = 0u;
    uint32_t oldest_tick = UINT32_MAX;
    uint32_t now = (uint32_t)xTaskGetTickCount();

    for (i = 0u; i < PWOS_NODE_HOST_ADVERTISE_CACHE_SIZE; ++i) {
        if (g_host_advertise_seen[i].valid != 0u &&
            g_host_advertise_seen[i].seq == seq &&
            g_host_advertise_seen[i].epoch == advertise->epoch &&
            uid_equal(g_host_advertise_seen[i].host_uid, advertise->host_uid)) {
            g_host_advertise_seen[i].last_tick = now;
            return 0;
        }
        if (g_host_advertise_seen[i].valid == 0u) {
            replace_index = i;
            oldest_tick = 0u;
            break;
        }
        if (g_host_advertise_seen[i].last_tick < oldest_tick) {
            replace_index = i;
            oldest_tick = g_host_advertise_seen[i].last_tick;
        }
    }

    memset(&g_host_advertise_seen[replace_index], 0,
        sizeof(g_host_advertise_seen[replace_index]));
    g_host_advertise_seen[replace_index].valid = 1u;
    g_host_advertise_seen[replace_index].seq = seq;
    g_host_advertise_seen[replace_index].epoch = advertise->epoch;
    memcpy(g_host_advertise_seen[replace_index].host_uid,
        advertise->host_uid,
        sizeof(g_host_advertise_seen[replace_index].host_uid));
    g_host_advertise_seen[replace_index].last_tick = now;
    return 1;
}

static void flood_host_advertise(
    const pwos_link_frame_view_t *view,
    uint8_t ingress_port)
{
    pwos_port_snapshot_t ports[PWOS_UART_DMA_MAX_PORTS];
    size_t count;
    size_t i;

    if (view == NULL || view->ttl <= 1u) {
        return;
    }

    count = pwos_port_manager_get_snapshot(ports, PWOS_UART_DMA_MAX_PORTS);
    for (i = 0u; i < count; ++i) {
        if (ports[i].id == ingress_port ||
            ports[i].peer_role != PWOS_PORT_PEER_NODE ||
            ports[i].last_rx_tick == 0u ||
            ports[i].state == PWOS_PORT_DISABLED ||
            ports[i].state == PWOS_PORT_SUSPECT ||
            ports[i].state == PWOS_PORT_QUARANTINED) {
            continue;
        }
        if (forward_view(view, ports[i].id) == 0) {
            ++g_stats.host_advertise_forward_tx;
        }
    }
}

static int send_register(uint8_t upstream_port)
{
    pwos_mesh2_node_register_t msg;
    uint8_t payload[PWOS_MESH2_NODE_REGISTER_PAYLOAD_LEN];
    size_t payload_len = 0u;

    memset(&msg, 0, sizeof(msg));
    msg.uid[0] = g_local_uid[0];
    msg.uid[1] = g_local_uid[1];
    msg.uid[2] = g_local_uid[2];
    msg.boot_id = g_local_boot_id;
    msg.caps = PWOS_MESH2_NODE_CAP_RELAY;
    msg.upstream_port = upstream_port;

    if (pwos_mesh2_encode_node_register(
            &msg, payload, sizeof(payload), &payload_len) != PWOS_OK) {
        return -1;
    }
    if (send_encoded_control(
            (uint8_t)PWOS_LINK_TYPE_CTRL_NODE_REGISTER,
            PWOS_LINK_ADDR_HOST,
            upstream_port,
            payload,
            (uint16_t)payload_len) != 0) {
        return -1;
    }

    g_last_register_tick = (uint32_t)xTaskGetTickCount();
    g_state = PWOS_NODE_REGISTERING;
    ++g_stats.register_tx;
    return 0;
}

static int send_lease_renew(void)
{
    pwos_mesh2_lease_renew_t msg;
    uint8_t payload[PWOS_MESH2_LEASE_RENEW_PAYLOAD_LEN];
    size_t payload_len = 0u;

    memset(&msg, 0, sizeof(msg));
    msg.uid[0] = g_local_uid[0];
    msg.uid[1] = g_local_uid[1];
    msg.uid[2] = g_local_uid[2];
    msg.boot_id = g_local_boot_id;
    msg.lease_epoch = g_lease_epoch;
    msg.addr = g_local_addr;

    if (pwos_mesh2_encode_lease_renew(
            &msg, payload, sizeof(payload), &payload_len) != PWOS_OK) {
        return -1;
    }
    if (send_encoded_control(
            (uint8_t)PWOS_LINK_TYPE_CTRL_LEASE_RENEW,
            PWOS_LINK_ADDR_HOST,
            g_upstream_port,
            payload,
            (uint16_t)payload_len) != 0) {
        return -1;
    }

    g_last_renew_tick = (uint32_t)xTaskGetTickCount();
    ++g_stats.renew_tx;
    return 0;
}

static void send_link_state_snapshot(void)
{
    pwos_port_snapshot_t ports[PWOS_UART_DMA_MAX_PORTS];
    size_t count;
    size_t i;

    count = pwos_port_manager_get_snapshot(ports, PWOS_UART_DMA_MAX_PORTS);
    for (i = 0u; i < count; ++i) {
        pwos_mesh2_link_state_t msg;
        uint8_t payload[PWOS_MESH2_LINK_STATE_PAYLOAD_LEN];
        size_t payload_len = 0u;

        if (ports[i].peer_role == PWOS_PORT_PEER_UNKNOWN ||
            ports[i].last_rx_tick == 0u) {
            continue;
        }

        memset(&msg, 0, sizeof(msg));
        msg.local_addr = g_local_addr;
        msg.local_port = ports[i].id;
        msg.peer_addr = ports[i].peer_addr;
        msg.peer_port = ports[i].peer_port_id;
        msg.flags = PWOS_MESH2_LINK_FLAG_UP;
        msg.metric = 1u;
        msg.peer_boot_id = ports[i].peer_boot_id;
        msg.peer_uid[0] = ports[i].peer_uid[0];
        msg.peer_uid[1] = ports[i].peer_uid[1];
        msg.peer_uid[2] = ports[i].peer_uid[2];

        if (pwos_mesh2_encode_link_state(
                &msg, payload, sizeof(payload), &payload_len) != PWOS_OK) {
            continue;
        }
        if (send_encoded_control(
                (uint8_t)PWOS_LINK_TYPE_CTRL_LINK_STATE,
                PWOS_LINK_ADDR_HOST,
                g_upstream_port,
                payload,
                (uint16_t)payload_len) == 0) {
            ++g_stats.link_state_tx;
        }
    }

    g_last_link_state_tick = (uint32_t)xTaskGetTickCount();
}

static void apply_self_assign(const pwos_mesh2_addr_assign_t *assign, uint8_t ingress_port)
{
    if (assign == NULL || (assign->flags & PWOS_MESH2_ASSIGN_FLAG_OK) == 0u) {
        return;
    }

    g_local_addr = assign->addr;
    g_upstream_port = ingress_port;
    g_lease_epoch = assign->lease_epoch;
    g_lease_ms = assign->lease_ms == 0u ? PWOS_NODE_DEFAULT_LEASE_MS : assign->lease_ms;
    g_state = PWOS_NODE_ASSIGNED;

    clear_routes();
    pwos_port_manager_mark_upstream(ingress_port);
    pwos_port_manager_set_mesh_assigned(1u, g_local_addr);
    ++g_stats.assign_rx;
}

static int apply_route_update(const pwos_mesh2_route_update_t *update)
{
    pwos_node_route_t *route;
    uint8_t port_id;

    if (update == NULL || update->dst == g_local_addr) {
        return -1;
    }

    taskENTER_CRITICAL();
    route = find_route(update->dst);
    if (route != NULL && update->route_version < route->route_version) {
        taskEXIT_CRITICAL();
        return 0;
    }

    if (update->action == PWOS_MESH2_ROUTE_DELETE) {
        if (route != NULL) {
            route->valid = 0u;
        }
        taskEXIT_CRITICAL();
        ++g_stats.route_update_rx;
        return 0;
    }
    taskEXIT_CRITICAL();

    if (resolve_next_hop_port(update->next_hop, &port_id) != 0) {
        ++g_stats.drop_no_route;
        return -1;
    }

    taskENTER_CRITICAL();
    route = alloc_route(update->dst);
    if (route != NULL) {
        route->next_hop = update->next_hop;
        route->port_id = port_id;
        route->metric = update->metric;
        route->route_version = update->route_version;
    }
    taskEXIT_CRITICAL();

    ++g_stats.route_update_rx;
    return route == NULL ? -1 : 0;
}

static int handle_addr_assign(
    const pwos_link_frame_view_t *view,
    uint8_t ingress_port)
{
    pwos_mesh2_addr_assign_t assign;
    uint8_t pending_port;

    if (pwos_mesh2_decode_addr_assign(
            view->payload, view->payload_len, &assign) != PWOS_OK) {
        ++g_stats.bad_ctrl_frames;
        return 1;
    }

    if (is_local_identity(assign.uid, assign.boot_id)) {
        apply_self_assign(&assign, ingress_port);
        return 1;
    }

    if (take_pending_assign(&assign, &pending_port) == 0) {
        set_direct_route(assign.addr, pending_port);
        if (forward_view(view, pending_port) == 0) {
            ++g_stats.relay_assign_forward_tx;
        } else {
            ++g_stats.forward_fail;
        }
        return 1;
    }

    ++g_stats.drop_no_route;
    return 1;
}

static int handle_lease_ack(
    const pwos_link_frame_view_t *view,
    uint8_t ingress_port)
{
    pwos_mesh2_lease_ack_t ack;

    if (!control_from_authority(ingress_port)) {
        return 1;
    }
    if (pwos_mesh2_decode_lease_ack(view->payload, view->payload_len, &ack) != PWOS_OK) {
        ++g_stats.bad_ctrl_frames;
        return 1;
    }
    if (ack.addr == g_local_addr && is_local_identity(ack.uid, ack.boot_id)) {
        g_lease_epoch = ack.lease_epoch;
        g_lease_ms = ack.lease_ms == 0u ? PWOS_NODE_DEFAULT_LEASE_MS : ack.lease_ms;
        g_state = PWOS_NODE_ASSIGNED;
        ++g_stats.lease_ack_rx;
    }
    return 1;
}

static int handle_node_register(
    const pwos_link_frame_view_t *view,
    uint8_t ingress_port)
{
    pwos_mesh2_node_register_t reg;

    if (pwos_mesh2_decode_node_register(view->payload, view->payload_len, &reg) != PWOS_OK) {
        ++g_stats.bad_ctrl_frames;
        return 1;
    }

    ++g_stats.relay_register_rx;
    if (g_state != PWOS_NODE_ASSIGNED) {
        ++g_stats.relay_register_drop_not_assigned;
        ++g_stats.drop_no_route;
        return 1;
    }

    record_pending_register(&reg, ingress_port);
    if (forward_view(view, g_upstream_port) == 0) {
        ++g_stats.relay_register_forward_tx;
    } else {
        ++g_stats.forward_fail;
    }
    return 1;
}

static int handle_route_update(
    const pwos_link_frame_view_t *view,
    uint8_t ingress_port)
{
    pwos_mesh2_route_update_t update;

    if (!control_from_authority(ingress_port)) {
        return 1;
    }
    if (pwos_mesh2_decode_route_update(view->payload, view->payload_len, &update) != PWOS_OK) {
        ++g_stats.bad_ctrl_frames;
        return 1;
    }
    (void)apply_route_update(&update);
    return 1;
}

static int handle_host_advertise(
    const pwos_link_frame_view_t *view,
    uint8_t ingress_port)
{
    pwos_mesh2_host_advertise_t advertise;
    int better;
    int first_seen;
    uint8_t same_authority;
    uint8_t better_path;
    uint32_t now;

    if (pwos_mesh2_decode_host_advertise(
            view->payload, view->payload_len, &advertise) != PWOS_OK ||
        advertise.cluster_id != PWOS_NODE_CLUSTER_ID) {
        ++g_stats.bad_ctrl_frames;
        return 1;
    }
    ++g_stats.host_advertise_rx;
    first_seen = remember_host_advertise(&advertise, view->seq);
    if (first_seen == 0) {
        ++g_stats.host_advertise_duplicate_rx;
    }

    if (advertise.role == PWOS_MESH2_HOST_ROLE_LEADER) {
        now = (uint32_t)xTaskGetTickCount();
        better = g_authority_valid == 0u ? 1 : host_candidate_compare(
            advertise.epoch, advertise.priority, advertise.host_uid);
        same_authority = g_authority_valid != 0u && better == 0;
        better_path = same_authority != 0u && view->ttl > g_authority_ttl;

        /*
         * 同一 leader 的等价副本不能切换 upstream，否则三角环中每个周期都会
         * 在两个入口间抖动。只有 TTL 更大（路径更短）时才允许切换。
         */
        if (better > 0 || better_path != 0u) {
            uint8_t must_rebind = g_state == PWOS_NODE_ASSIGNED &&
                ((g_authority_valid == 0u &&
                  g_upstream_port != ingress_port) ||
                 (g_authority_valid != 0u &&
                  (better != 0 || g_authority_port != ingress_port)));

            g_authority_valid = 1u;
            g_authority_port = ingress_port;
            memcpy(g_authority_uid, advertise.host_uid, sizeof(g_authority_uid));
            g_authority_epoch = advertise.epoch;
            g_authority_priority = advertise.priority;
            g_authority_ttl = view->ttl;
            g_authority_last_tick = now;
            if (must_rebind != 0u) {
                /* leader 或到 leader 的路径改变后，必须重新获取 lease 和路由。 */
                g_state = PWOS_NODE_OFFLINE;
                g_local_addr = PWOS_LINK_ADDR_UNASSIGNED;
                g_upstream_port = PWOS_LINK_ADDR_UNASSIGNED;
                g_lease_epoch = 0u;
                clear_routes();
                pwos_port_manager_set_mesh_assigned(
                    0u, PWOS_LINK_ADDR_UNASSIGNED);
            }
        } else if (same_authority != 0u &&
                   ingress_port == g_authority_port) {
            g_authority_last_tick = now;
            if (view->ttl > g_authority_ttl) {
                g_authority_ttl = view->ttl;
            }
        }
    }

    if (first_seen != 0) {
        flood_host_advertise(view, ingress_port);
    }
    return 1;
}

static int route_or_drop(const pwos_link_frame_view_t *view)
{
    uint8_t port_id;

    if (view->dst == PWOS_LINK_ADDR_HOST && g_state == PWOS_NODE_ASSIGNED) {
        (void)forward_view(view, g_upstream_port);
        return 1;
    }

    if (pwos_node_control_route_lookup(view->dst, &port_id) == 0) {
        (void)forward_view(view, port_id);
    } else {
        ++g_stats.drop_no_route;
    }
    return 1;
}

void pwos_node_control_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_routes, 0, sizeof(g_routes));
    memset(g_pending, 0, sizeof(g_pending));

    g_local_uid[0] = HAL_GetUIDw0();
    g_local_uid[1] = HAL_GetUIDw1();
    g_local_uid[2] = HAL_GetUIDw2();
    g_local_boot_id = g_local_uid[0] ^ g_local_uid[1] ^ g_local_uid[2] ^ 0x504F5753u;
    g_local_addr = PWOS_LINK_ADDR_UNASSIGNED;
    g_upstream_port = PWOS_LINK_ADDR_UNASSIGNED;
    g_lease_epoch = 0u;
    g_lease_ms = PWOS_NODE_DEFAULT_LEASE_MS;
    g_last_register_tick = 0u;
    g_last_renew_tick = 0u;
    g_last_link_state_tick = 0u;
    g_next_seq = 0u;
    g_state = PWOS_NODE_OFFLINE;
    g_authority_valid = 0u;
    g_authority_port = PWOS_LINK_ADDR_UNASSIGNED;
    memset(g_authority_uid, 0, sizeof(g_authority_uid));
    g_authority_epoch = 0u;
    g_authority_priority = 0u;
    g_authority_ttl = 0u;
    g_authority_last_tick = 0u;
    memset(g_host_advertise_seen, 0, sizeof(g_host_advertise_seen));
    pwos_port_manager_set_mesh_assigned(0u, PWOS_LINK_ADDR_UNASSIGNED);
}

void pwos_node_control_tick(void)
{
    uint8_t upstream_port;
    uint32_t now;
    uint32_t renew_interval;

    now = (uint32_t)xTaskGetTickCount();

    if (g_authority_valid != 0u &&
        (uint32_t)(now - g_authority_last_tick) >=
            pdMS_TO_TICKS(PWOS_NODE_HOST_ADVERTISE_TIMEOUT_MS)) {
        g_authority_valid = 0u;
        g_authority_port = PWOS_LINK_ADDR_UNASSIGNED;
        g_authority_ttl = 0u;
        if (g_state == PWOS_NODE_ASSIGNED) {
            g_state = PWOS_NODE_OFFLINE;
            g_local_addr = PWOS_LINK_ADDR_UNASSIGNED;
            g_upstream_port = PWOS_LINK_ADDR_UNASSIGNED;
            g_lease_epoch = 0u;
            clear_routes();
            pwos_port_manager_set_mesh_assigned(
                0u, PWOS_LINK_ADDR_UNASSIGNED);
        }
    }

    if (g_state != PWOS_NODE_ASSIGNED) {
        if ((g_last_register_tick == 0u ||
             (uint32_t)(now - g_last_register_tick) >= pdMS_TO_TICKS(PWOS_NODE_REGISTER_INTERVAL_MS)) &&
            select_control_upstream(&upstream_port) == 0) {
            (void)send_register(upstream_port);
        }
        g_stats.state = g_state;
        g_stats.local_addr = g_local_addr;
        g_stats.upstream_port = g_upstream_port;
        g_stats.authority_valid = g_authority_valid;
        g_stats.authority_port = g_authority_port;
        memcpy(g_stats.authority_uid, g_authority_uid, sizeof(g_stats.authority_uid));
        g_stats.authority_epoch = g_authority_epoch;
        g_stats.authority_priority = g_authority_priority;
        g_stats.authority_ttl = g_authority_ttl;
        return;
    }

    renew_interval = g_lease_ms / 2u;
    if (renew_interval < 1000u) {
        renew_interval = 1000u;
    }

    if (g_last_renew_tick == 0u ||
        (uint32_t)(now - g_last_renew_tick) >= pdMS_TO_TICKS(renew_interval)) {
        (void)send_lease_renew();
    }

    if (g_last_link_state_tick == 0u ||
        (uint32_t)(now - g_last_link_state_tick) >= pdMS_TO_TICKS(PWOS_NODE_LINK_STATE_INTERVAL_MS)) {
        send_link_state_snapshot();
    }

    g_stats.state = g_state;
    g_stats.local_addr = g_local_addr;
    g_stats.upstream_port = g_upstream_port;
    g_stats.local_uid[0] = g_local_uid[0];
    g_stats.local_uid[1] = g_local_uid[1];
    g_stats.local_uid[2] = g_local_uid[2];
    g_stats.local_boot_id = g_local_boot_id;
    g_stats.lease_epoch = g_lease_epoch;
    g_stats.lease_ms = g_lease_ms;
    g_stats.last_register_tick = g_last_register_tick;
    g_stats.last_renew_tick = g_last_renew_tick;
    g_stats.authority_valid = g_authority_valid;
    g_stats.authority_port = g_authority_port;
    memcpy(g_stats.authority_uid, g_authority_uid, sizeof(g_stats.authority_uid));
    g_stats.authority_epoch = g_authority_epoch;
    g_stats.authority_priority = g_authority_priority;
    g_stats.authority_ttl = g_authority_ttl;
}

int pwos_node_control_handle_rx(pwos_frame_block_t *block)
{
    pwos_link_frame_view_t view;

    if (block == NULL || block->len == 0u) {
        return 0;
    }
    if (pwos_link_decode(block->data, block->len, &view) != PWOS_OK) {
        ++g_stats.bad_ctrl_frames;
        return 1;
    }

    if (view.src != PWOS_LINK_ADDR_UNASSIGNED &&
        view.src != PWOS_LINK_ADDR_HOST &&
        view.src != g_local_addr) {
        set_direct_route(view.src, block->port_id);
    }

    if (view.dst != g_local_addr &&
        view.dst != PWOS_LINK_ADDR_UNASSIGNED &&
        view.dst != PWOS_LINK_ADDR_HOST) {
        return route_or_drop(&view);
    }

    switch (view.type) {
    case PWOS_LINK_TYPE_CTRL_NODE_REGISTER:
        return handle_node_register(&view, block->port_id);
    case PWOS_LINK_TYPE_CTRL_ADDR_ASSIGN:
        if (!control_from_authority(block->port_id)) return 1;
        return handle_addr_assign(&view, block->port_id);
    case PWOS_LINK_TYPE_CTRL_LEASE_ACK:
        return handle_lease_ack(&view, block->port_id);
    case PWOS_LINK_TYPE_CTRL_ROUTE_UPDATE:
        return handle_route_update(&view, block->port_id);
    case PWOS_LINK_TYPE_CTRL_HOST_ADVERTISE:
        return handle_host_advertise(&view, block->port_id);
    case PWOS_LINK_TYPE_CTRL_LINK_STATE:
    case PWOS_LINK_TYPE_CTRL_LEASE_RENEW:
        return route_or_drop(&view);
    default:
        if (pwos_link_type_is_data(view.type) && view.dst != g_local_addr) {
            return route_or_drop(&view);
        }
        return 0;
    }
}

int pwos_node_control_route_lookup(uint8_t dst, uint8_t *out_port_id)
{
    pwos_node_route_t *route;

    if (out_port_id == NULL || dst == g_local_addr) {
        return -1;
    }
    if (dst == PWOS_LINK_ADDR_HOST && g_state == PWOS_NODE_ASSIGNED) {
        *out_port_id = g_upstream_port;
        return 0;
    }

    route = find_route(dst);
    if (route == NULL) {
        return -1;
    }
    *out_port_id = route->port_id;
    return 0;
}

int pwos_node_control_send_data(
    uint8_t type,
    uint8_t dst,
    const uint8_t *payload,
    uint16_t payload_len)
{
    uint8_t port_id;

    if (!pwos_link_type_is_data(type) ||
        (payload_len > 0u && payload == NULL) ||
        pwos_node_control_route_lookup(dst, &port_id) != 0) {
        ++g_stats.local_data_tx_fail;
        return -1;
    }

    if (send_encoded_data(type, dst, port_id, payload, payload_len) != 0) {
        ++g_stats.local_data_tx_fail;
        return -1;
    }

    ++g_stats.local_data_tx;
    return 0;
}

void pwos_node_control_get_snapshot(pwos_node_control_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL();
    g_stats.state = g_state;
    g_stats.local_addr = g_local_addr;
    g_stats.upstream_port = g_upstream_port;
    g_stats.lease_epoch = g_lease_epoch;
    g_stats.lease_ms = g_lease_ms;
    g_stats.last_register_tick = g_last_register_tick;
    g_stats.last_renew_tick = g_last_renew_tick;
    *out = g_stats;
    taskEXIT_CRITICAL();
}

size_t pwos_node_control_get_routes(
    pwos_node_route_snapshot_t *out,
    size_t out_capacity)
{
    size_t i;
    size_t count = 0u;

    if (out == NULL || out_capacity == 0u) {
        return 0u;
    }

    taskENTER_CRITICAL();
    for (i = 0u; i < PWOS_NODE_CONTROL_MAX_ROUTES && count < out_capacity; ++i) {
        if (g_routes[i].valid == 0u) {
            continue;
        }
        out[count].valid = g_routes[i].valid;
        out[count].dst = g_routes[i].dst;
        out[count].next_hop = g_routes[i].next_hop;
        out[count].port_id = g_routes[i].port_id;
        out[count].metric = g_routes[i].metric;
        out[count].route_version = g_routes[i].route_version;
        ++count;
    }
    taskEXIT_CRITICAL();

    return count;
}

const char *pwos_node_state_name(pwos_node_state_t state)
{
    switch (state) {
    case PWOS_NODE_OFFLINE:
        return "OFFLINE";
    case PWOS_NODE_REGISTERING:
        return "REGISTERING";
    case PWOS_NODE_ASSIGNED:
        return "ASSIGNED";
    case PWOS_NODE_LIMITED:
        return "LIMITED";
    default:
        return "UNKNOWN";
    }
}
