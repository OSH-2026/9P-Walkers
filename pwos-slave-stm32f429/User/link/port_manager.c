#include "port_manager.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"

#include "pwos_link_frame.h"
#include "pwos_queues.h"
#include "uart_dma_port.h"

#define PWOS_PORT_HELLO_PAYLOAD_VERSION 1u
#define PWOS_PORT_HELLO_PAYLOAD_LEN 24u
#define PWOS_PORT_HELLO_INTERVAL_MS 500u
#define PWOS_PORT_SUSPECT_MS 2000u

typedef struct {
    uint8_t initialized;
    uint8_t id;
    const char *name;
    pwos_port_state_t state;
    pwos_port_peer_role_t peer_role;
    uint8_t peer_port_id;
    uint8_t peer_addr;  /* mesh 地址，从带 src 的帧学习 */
    uint8_t peer_caps;
    uint32_t peer_boot_id;
    uint32_t peer_uid[3];
    uint32_t last_rx_tick;
    uint32_t last_tx_tick;
    uint32_t hello_tx;
    uint32_t hello_rx;
    uint32_t hello_ack_tx;
    uint32_t hello_ack_rx;
    uint32_t bad_link_frames;
} pwos_port_runtime_t;

static pwos_port_runtime_t g_ports[PWOS_UART_DMA_MAX_PORTS];
static size_t g_port_count;
static uint32_t g_local_uid[3];
static uint32_t g_local_boot_id;
static uint16_t g_next_seq;
static uint8_t g_mesh_assigned;
static uint8_t g_local_addr;

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

static pwos_port_runtime_t *find_port(uint8_t port_id)
{
    size_t i;

    for (i = 0u; i < g_port_count; ++i) {
        if (g_ports[i].id == port_id) {
            return &g_ports[i];
        }
    }
    return NULL;
}

static void encode_hello_payload(uint8_t local_port_id, uint8_t *payload)
{
    uint8_t caps;

    caps = PWOS_PORT_CAP_RELAY;
    if (g_mesh_assigned != 0u) {
        caps |= PWOS_PORT_CAP_UPSTREAM_REACHABLE;
    }

    memset(payload, 0, PWOS_PORT_HELLO_PAYLOAD_LEN);
    payload[0] = PWOS_PORT_HELLO_PAYLOAD_VERSION;
    payload[1] = (uint8_t)PWOS_PORT_PEER_NODE;
    payload[2] = local_port_id;
    payload[3] = caps;
    put_le32(payload + 4u, g_local_boot_id);
    put_le32(payload + 8u, g_local_uid[0]);
    put_le32(payload + 12u, g_local_uid[1]);
    put_le32(payload + 16u, g_local_uid[2]);
    put_le32(payload + 20u, (uint32_t)xTaskGetTickCount());
}

static int decode_hello_payload(
    const pwos_link_frame_view_t *view,
    pwos_port_peer_role_t *out_role,
    uint8_t *out_peer_port_id,
    uint8_t *out_caps,
    uint32_t out_uid[3],
    uint32_t *out_boot_id)
{
    const uint8_t *payload;

    if (view == NULL || view->payload == NULL ||
        view->payload_len != PWOS_PORT_HELLO_PAYLOAD_LEN ||
        out_role == NULL || out_peer_port_id == NULL ||
        out_caps == NULL || out_uid == NULL || out_boot_id == NULL) {
        return -1;
    }

    payload = view->payload;
    if (payload[0] != PWOS_PORT_HELLO_PAYLOAD_VERSION) {
        return -1;
    }
    if (payload[1] != (uint8_t)PWOS_PORT_PEER_NODE &&
        payload[1] != (uint8_t)PWOS_PORT_PEER_COORDINATOR) {
        return -1;
    }

    *out_role = (pwos_port_peer_role_t)payload[1];
    *out_peer_port_id = payload[2];
    *out_caps = payload[3];
    *out_boot_id = get_le32(payload + 4u);
    out_uid[0] = get_le32(payload + 8u);
    out_uid[1] = get_le32(payload + 12u);
    out_uid[2] = get_le32(payload + 16u);
    return 0;
}

static int send_link_hello(uint8_t port_id, uint8_t frame_type)
{
    uint8_t payload[PWOS_PORT_HELLO_PAYLOAD_LEN];
    pwos_frame_block_t *block;
    size_t frame_len = 0u;
    pwos_status_t status;

    block = pwos_frame_pool_alloc();
    if (block == NULL) {
        return -1;
    }

    encode_hello_payload(port_id, payload);
    status = pwos_link_encode(
        frame_type,
        0u,
        g_mesh_assigned ? g_local_addr : PWOS_LINK_ADDR_UNASSIGNED,
        PWOS_LINK_ADDR_UNASSIGNED,
        1u,
        ++g_next_seq,
        0u,
        payload,
        (uint16_t)sizeof(payload),
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

static void update_state_from_peer(
    pwos_port_runtime_t *port,
    pwos_port_peer_role_t role,
    uint8_t peer_port_id,
    uint8_t caps,
    const uint32_t uid[3],
    uint32_t boot_id)
{
    if (port == NULL || uid == NULL) {
        return;
    }

    port->peer_role = role;
    port->peer_port_id = peer_port_id;
    port->peer_caps = caps;
    port->peer_boot_id = boot_id;
    port->peer_uid[0] = uid[0];
    port->peer_uid[1] = uid[1];
    port->peer_uid[2] = uid[2];
    port->last_rx_tick = (uint32_t)xTaskGetTickCount();

    if (role == PWOS_PORT_PEER_COORDINATOR || (caps & PWOS_PORT_CAP_COORDINATOR) != 0u) {
        port->state = PWOS_PORT_HOST_CANDIDATE;
    } else {
        port->state = PWOS_PORT_PEER;
    }
}

void pwos_port_manager_init(void)
{
    size_t i;
    size_t count;

    memset(g_ports, 0, sizeof(g_ports));
    g_local_uid[0] = HAL_GetUIDw0();
    g_local_uid[1] = HAL_GetUIDw1();
    g_local_uid[2] = HAL_GetUIDw2();
    g_local_boot_id = g_local_uid[0] ^ g_local_uid[1] ^ g_local_uid[2] ^ 0x504F5753u;
    g_next_seq = 0u;
    g_mesh_assigned = 0u;
    g_local_addr = PWOS_LINK_ADDR_UNASSIGNED;

    count = pwos_uart_dma_port_count();
    if (count > (sizeof(g_ports) / sizeof(g_ports[0]))) {
        count = sizeof(g_ports) / sizeof(g_ports[0]);
    }
    g_port_count = count;

    for (i = 0u; i < g_port_count; ++i) {
        const pwos_uart_dma_port_desc_t *desc = pwos_uart_dma_port_desc(i);
        if (desc == NULL) {
            continue;
        }
        g_ports[i].initialized = 1u;
        g_ports[i].id = desc->id;
        g_ports[i].name = desc->name;
        g_ports[i].state = PWOS_PORT_PROBING;
        g_ports[i].peer_role = PWOS_PORT_PEER_UNKNOWN;
    }
}

void pwos_port_manager_tick(void)
{
    size_t i;
    uint32_t now = (uint32_t)xTaskGetTickCount();

    for (i = 0u; i < g_port_count; ++i) {
        pwos_port_runtime_t *port = &g_ports[i];

        if (port->initialized == 0u || port->state == PWOS_PORT_DISABLED ||
            port->state == PWOS_PORT_QUARANTINED) {
            continue;
        }

        if (port->last_rx_tick != 0u &&
            (uint32_t)(now - port->last_rx_tick) > pdMS_TO_TICKS(PWOS_PORT_SUSPECT_MS)) {
            port->state = PWOS_PORT_SUSPECT;
            port->peer_role = PWOS_PORT_PEER_UNKNOWN;
        }

        if (port->last_tx_tick == 0u ||
            (uint32_t)(now - port->last_tx_tick) >= pdMS_TO_TICKS(PWOS_PORT_HELLO_INTERVAL_MS)) {
            if (send_link_hello(port->id, (uint8_t)PWOS_LINK_TYPE_LINK_HELLO) == 0) {
                port->last_tx_tick = now;
                ++port->hello_tx;
                if (port->state == PWOS_PORT_SUSPECT) {
                    port->state = PWOS_PORT_PROBING;
                }
            }
        }
    }
}

int pwos_port_manager_handle_rx(pwos_frame_block_t *block)
{
    pwos_link_frame_view_t view;
    pwos_port_runtime_t *port;
    pwos_port_peer_role_t role;
    uint8_t peer_port_id;
    uint8_t peer_addr;  /* mesh 地址，从带 src 的帧学习 */
    uint8_t caps;
    uint32_t peer_uid[3];
    uint32_t peer_boot_id;

    if (block == NULL || block->len == 0u) {
        return 0;
    }

    if (pwos_link_decode(block->data, block->len, &view) != PWOS_OK) {
        return 0;
    }

    if (view.type != (uint8_t)PWOS_LINK_TYPE_LINK_HELLO &&
        view.type != (uint8_t)PWOS_LINK_TYPE_LINK_HELLO_ACK) {
        return 0;
    }

    port = find_port(block->port_id);
    if (port == NULL) {
        return 1;
    }

    if (decode_hello_payload(&view, &role, &peer_port_id, &caps, peer_uid, &peer_boot_id) != 0) {
        ++port->bad_link_frames;
        return 1;
    }

    update_state_from_peer(port, role, peer_port_id, caps, peer_uid, peer_boot_id);

    if (view.type == (uint8_t)PWOS_LINK_TYPE_LINK_HELLO) {
        ++port->hello_rx;
        if (view.src != PWOS_LINK_ADDR_UNASSIGNED && view.src != PWOS_LINK_ADDR_HOST) { pwos_port_manager_set_peer_addr(port->id, view.src); }
        if (send_link_hello(port->id, (uint8_t)PWOS_LINK_TYPE_LINK_HELLO_ACK) == 0) {
            ++port->hello_ack_tx;
        }
    } else {
        if (view.src != PWOS_LINK_ADDR_UNASSIGNED && view.src != PWOS_LINK_ADDR_HOST) { pwos_port_manager_set_peer_addr(port->id, view.src); }
        ++port->hello_ack_rx;
    }

    return 1;
}

int pwos_port_manager_select_upstream(uint8_t *out_port_id)
{
    size_t i;

    if (out_port_id == NULL) {
        return -1;
    }

    taskENTER_CRITICAL();
    for (i = 0u; i < g_port_count; ++i) {
        if (g_ports[i].initialized != 0u &&
            g_ports[i].state == PWOS_PORT_UPSTREAM) {
            *out_port_id = g_ports[i].id;
            taskEXIT_CRITICAL();
            return 0;
        }
    }
    for (i = 0u; i < g_port_count; ++i) {
        if (g_ports[i].initialized != 0u &&
            g_ports[i].state == PWOS_PORT_HOST_CANDIDATE) {
            *out_port_id = g_ports[i].id;
            taskEXIT_CRITICAL();
            return 0;
        }
    }
    for (i = 0u; i < g_port_count; ++i) {
        if (g_ports[i].initialized != 0u &&
            g_ports[i].state == PWOS_PORT_PEER &&
            (g_ports[i].peer_caps & PWOS_PORT_CAP_UPSTREAM_REACHABLE) != 0u) {
            *out_port_id = g_ports[i].id;
            taskEXIT_CRITICAL();
            return 0;
        }
    }
    taskEXIT_CRITICAL();
    return -1;
}

void pwos_port_manager_mark_upstream(uint8_t port_id)
{
    size_t i;

    taskENTER_CRITICAL();
    for (i = 0u; i < g_port_count; ++i) {
        if (g_ports[i].initialized == 0u) {
            continue;
        }
        if (g_ports[i].id == port_id) {
            g_ports[i].state = PWOS_PORT_UPSTREAM;
        } else if (g_ports[i].state == PWOS_PORT_UPSTREAM) {
            g_ports[i].state = PWOS_PORT_PEER;
        }
    }
    taskEXIT_CRITICAL();
}

void pwos_port_manager_set_peer_addr(uint8_t port_id, uint8_t peer_addr)
{
    size_t i;

    taskENTER_CRITICAL();
    for (i = 0u; i < g_port_count; ++i) {
        if (g_ports[i].initialized != 0u && g_ports[i].id == port_id) {
            g_ports[i].peer_addr = peer_addr;
            break;
        }
    }
    taskEXIT_CRITICAL();
}

int pwos_port_manager_find_port_by_peer_addr(uint8_t peer_addr)
{
    size_t i;
    int result = -1;

    if (peer_addr == PWOS_LINK_ADDR_UNASSIGNED || peer_addr == PWOS_LINK_ADDR_HOST) {
        return -1;
    }
    taskENTER_CRITICAL();
    for (i = 0u; i < g_port_count; ++i) {
        if (g_ports[i].initialized != 0u && g_ports[i].peer_addr == peer_addr) {
            result = (int)g_ports[i].id;
            break;
        }
    }
    taskEXIT_CRITICAL();
    return result;
}

void pwos_port_manager_set_mesh_assigned(uint8_t assigned, uint8_t local_addr)
{
    size_t i;

    taskENTER_CRITICAL();
    g_mesh_assigned = assigned != 0u ? 1u : 0u;
    g_local_addr = assigned != 0u ? local_addr : PWOS_LINK_ADDR_UNASSIGNED;

    /*
     * mesh 地址状态改变后，HELLO capability 也会改变。
     * 清 last_tx_tick 让下一次 port_manager_tick 立即广播新能力，避免下游长时间
     * 看不到 UPSTREAM_REACHABLE。
     */
    for (i = 0u; i < g_port_count; ++i) {
        if (g_ports[i].initialized != 0u) {
            g_ports[i].last_tx_tick = 0u;
        }
    }
    (void)g_local_addr;
    taskEXIT_CRITICAL();
}

size_t pwos_port_manager_get_snapshot(
    pwos_port_snapshot_t *out,
    size_t out_capacity)
{
    size_t i;
    size_t count;

    if (out == NULL || out_capacity == 0u) {
        return 0u;
    }

    taskENTER_CRITICAL();
    count = g_port_count < out_capacity ? g_port_count : out_capacity;
    for (i = 0u; i < count; ++i) {
        out[i].id = g_ports[i].id;
        out[i].name = g_ports[i].name;
        out[i].state = g_ports[i].state;
        out[i].peer_role = g_ports[i].peer_role;
        out[i].peer_port_id = g_ports[i].peer_port_id;
        out[i].peer_addr = g_ports[i].peer_addr;
        out[i].peer_caps = g_ports[i].peer_caps;
        out[i].peer_boot_id = g_ports[i].peer_boot_id;
        out[i].peer_uid[0] = g_ports[i].peer_uid[0];
        out[i].peer_uid[1] = g_ports[i].peer_uid[1];
        out[i].peer_uid[2] = g_ports[i].peer_uid[2];
        out[i].last_rx_tick = g_ports[i].last_rx_tick;
        out[i].last_tx_tick = g_ports[i].last_tx_tick;
        out[i].hello_tx = g_ports[i].hello_tx;
        out[i].hello_rx = g_ports[i].hello_rx;
        out[i].hello_ack_tx = g_ports[i].hello_ack_tx;
        out[i].hello_ack_rx = g_ports[i].hello_ack_rx;
        out[i].bad_link_frames = g_ports[i].bad_link_frames;
    }
    taskEXIT_CRITICAL();

    return count;
}

const char *pwos_port_state_name(pwos_port_state_t state)
{
    switch (state) {
    case PWOS_PORT_DISABLED:
        return "DISABLED";
    case PWOS_PORT_PROBING:
        return "PROBING";
    case PWOS_PORT_LINK_UP:
        return "LINK_UP";
    case PWOS_PORT_HOST_CANDIDATE:
        return "HOST_CANDIDATE";
    case PWOS_PORT_UPSTREAM:
        return "UPSTREAM";
    case PWOS_PORT_PEER:
        return "PEER";
    case PWOS_PORT_SUSPECT:
        return "SUSPECT";
    case PWOS_PORT_QUARANTINED:
        return "QUARANTINED";
    default:
        return "UNKNOWN";
    }
}
