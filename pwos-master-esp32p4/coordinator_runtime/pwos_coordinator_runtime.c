#include "pwos_coordinator_runtime.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#include "cluster_vfs.h"
#include "job_command.h"
#include "job_manager.h"
#include "host_rpc_runtime.h"
#include "pwos_link_frame.h"
#include "pwos_link_parser.h"
#include "pwos_mesh2_control.h"
#include "pwos_rpc_protocol.h"
#include "pwos_job_protocol.h"
#include "rpc_client.h"
#include "session_manager.h"

#define PWOS_COORDINATOR_TASK_NAME "pwos_coord"
#define PWOS_COORDINATOR_TASK_STACK 4096
#define PWOS_COORDINATOR_TASK_PRIO 8
#define PWOS_COORDINATOR_PROBE_TASK_NAME "pwos_m9p_probe"
#define PWOS_COORDINATOR_PROBE_TASK_STACK 4096
#define PWOS_COORDINATOR_PROBE_TASK_PRIO 6
#define PWOS_COORDINATOR_TX_GUARD_US 2000u
#define PWOS_COORDINATOR_UART_RX_BUF_SIZE 2048
#define PWOS_COORDINATOR_UART_TX_BUF_SIZE 2048
#define PWOS_COORDINATOR_READ_SLICE_SIZE 128
#define PWOS_COORDINATOR_HELLO_INTERVAL_MS 500u
#define PWOS_COORDINATOR_REPORT_INTERVAL_MS 5000u
#define PWOS_COORDINATOR_HOST_ADVERTISE_INTERVAL_MS 1000u
#define PWOS_COORDINATOR_MINI9P_PROBE_INTERVAL_MS 10000u
#define PWOS_COORDINATOR_MINI9P_DEADLINE_MS 1000u
#define PWOS_COORDINATOR_MINI9P_HEALTH_READ_LEN 32u
#define PWOS_COORDINATOR_DEFAULT_TTL 8u
#define PWOS_COORDINATOR_HELLO_PAYLOAD_VERSION 1u
#define PWOS_COORDINATOR_HELLO_PAYLOAD_LEN 24u
#define PWOS_COORDINATOR_ROLE_COORDINATOR 2u
#define PWOS_COORDINATOR_CAP_COORDINATOR 0x01u
#define PWOS_COORDINATOR_CAP_RELAY 0x02u

typedef struct {
    uint8_t initialized;
    uint8_t task_started;
    uint8_t uart_port;
    uint8_t control_leader;
    uint16_t next_seq;
    uint32_t boot_id;
    uint32_t uid[3];
    pwos_link_parser_t parser;
    pwos_host_coordinator_t coordinator;
    pwos_session_manager_t sessions;
    pwos_rpc_client_t rpc_client;
    pwos_job_manager_t jobs;
    pwos_job_command_t job_command;
    pwos_cluster_vfs_t cluster_vfs;
    uint32_t mini9p_last_probe_tick[PWOS_HOST_COORDINATOR_MAX_NODES];
    pwos_coordinator_runtime_stats_t stats;
#ifdef ESP_PLATFORM
    TaskHandle_t task;
    TaskHandle_t probe_task;
    SemaphoreHandle_t tx_mutex;
    SemaphoreHandle_t coordinator_mutex;
    SemaphoreHandle_t session_mutex;
    SemaphoreHandle_t session_client_mutex[PWOS_SESSION_MANAGER_MAX_SESSIONS];
    SemaphoreHandle_t pending_event[PWOS_SESSION_MANAGER_MAX_PENDING];
    SemaphoreHandle_t vfs_mutex;
    SemaphoreHandle_t job_mutex;
#endif
} pwos_coordinator_runtime_t;

static pwos_coordinator_runtime_t g_runtime;

static int find_rpc_route(
    const char *target,
    pwos_cluster_vfs_route_t *out_route);

#ifdef ESP_PLATFORM
static const char *TAG = "pwos_coord";

static void coordinator_lock(pwos_coordinator_runtime_t *runtime)
{
    (void)xSemaphoreTake(runtime->coordinator_mutex, portMAX_DELAY);
}

static void coordinator_unlock(pwos_coordinator_runtime_t *runtime)
{
    (void)xSemaphoreGive(runtime->coordinator_mutex);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void encode_hello_payload(
    const pwos_coordinator_runtime_t *runtime,
    uint8_t local_port_id,
    uint8_t *payload)
{
    memset(payload, 0, PWOS_COORDINATOR_HELLO_PAYLOAD_LEN);

    /*
     * HELLO payload 与 STM32 port_manager 保持一致：
     * [version, role, local_port, caps, boot_id, uid0, uid1, uid2, tick]。
     */
    payload[0] = PWOS_COORDINATOR_HELLO_PAYLOAD_VERSION;
    payload[1] = PWOS_COORDINATOR_ROLE_COORDINATOR;
    payload[2] = local_port_id;
    payload[3] = PWOS_COORDINATOR_CAP_COORDINATOR | PWOS_COORDINATOR_CAP_RELAY;
    put_le32(payload + 4u, runtime->boot_id);
    put_le32(payload + 8u, runtime->uid[0]);
    put_le32(payload + 12u, runtime->uid[1]);
    put_le32(payload + 16u, runtime->uid[2]);
    put_le32(payload + 20u, (uint32_t)xTaskGetTickCount());
}

static uint8_t count_nodes(const pwos_host_coordinator_t *coordinator)
{
    size_t i;
    uint8_t count = 0u;

    if (coordinator == NULL) {
        return 0u;
    }

    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        if (coordinator->nodes[i].valid != 0u) {
            ++count;
        }
    }
    return count;
}

static void sync_cluster_vfs(pwos_coordinator_runtime_t *runtime)
{
    pwos_host_coordinator_t snapshot;
    int rc;

    if (runtime == NULL) {
        return;
    }

    coordinator_lock(runtime);
    snapshot = runtime->coordinator;
    coordinator_unlock(runtime);
    rc = pwos_cluster_vfs_sync_from_coordinator(&runtime->cluster_vfs, &snapshot);
    if (rc != 0) {
        runtime->stats.mini9p_last_error = (int32_t)rc;
        ESP_LOGW(TAG, "cluster_vfs sync failed rc=%d (%s)", rc, pwos_session_error_name(rc));
    }
}

static uint8_t local_host_is_control_leader(void)
{
    pwos_host_rpc_runtime_status_t host;

    memset(&host, 0, sizeof(host));
    pwos_host_rpc_runtime_get_status(&host);
    /* 无 IP/host_rpc 时保留单主离线模式。 */
    return host.initialized == 0u ||
        host.local_role == PWOS_HOST_ROLE_LEADER;
}

static void reconcile_control_role(pwos_coordinator_runtime_t *runtime)
{
    uint8_t leader;

    if (runtime == NULL) {
        return;
    }

    leader = local_host_is_control_leader();
    if (runtime->control_leader == leader) {
        runtime->stats.control_leader = leader;
        return;
    }

    runtime->control_leader = leader;
    runtime->stats.control_leader = leader;
    if (leader == 0u) {
        /*
         * 两台主机启动时会短暂都认为自己是 leader。角色收敛为 follower 后，
         * 清掉该窗口内分配的地址，避免同一 MCU 同时存在于两个地址域。
         */
        coordinator_lock(runtime);
        pwos_host_coordinator_init(&runtime->coordinator);
        runtime->stats.node_count = 0u;
        coordinator_unlock(runtime);
        memset(runtime->mini9p_last_probe_tick, 0,
            sizeof(runtime->mini9p_last_probe_tick));
        sync_cluster_vfs(runtime);
        ESP_LOGI(TAG, "control role=follower, local mesh ownership cleared");
    } else {
        ESP_LOGI(TAG, "control role=leader");
    }
}

static int send_payload(
    pwos_coordinator_runtime_t *runtime,
    uint8_t type,
    uint8_t dst,
    uint8_t ttl,
    const uint8_t *payload,
    uint16_t payload_len)
{
    uint8_t frame[PWOS_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    pwos_status_t status;
    int written;

    if (runtime == NULL || runtime->tx_mutex == NULL ||
        xSemaphoreTake(runtime->tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return PWOS_SESSION_ERR_QUEUE_FULL;
    }

    status = pwos_link_encode(
        type,
        0u,
        PWOS_LINK_ADDR_HOST,
        dst,
        ttl,
        ++runtime->next_seq,
        0u,
        payload,
        payload_len,
        frame,
        sizeof(frame),
        &frame_len);
    if (status != PWOS_OK) {
        ++runtime->stats.tx_errors;
        ESP_LOGW(TAG, "encode type=0x%02x failed: %s", type, pwos_status_string(status));
        (void)xSemaphoreGive(runtime->tx_mutex);
        return -1;
    }

    written = uart_write_bytes((uart_port_t)runtime->uart_port, frame, frame_len);
    if (written < 0 || (size_t)written != frame_len) {
        ++runtime->stats.tx_errors;
        ESP_LOGW(TAG, "uart write type=0x%02x failed written=%d len=%u",
            type,
            written,
            (unsigned)frame_len);
        (void)xSemaphoreGive(runtime->tx_mutex);
        return -1;
    }

    (void)uart_wait_tx_done((uart_port_t)runtime->uart_port, pdMS_TO_TICKS(100));
    /*
     * STM32 使用 DMA_NORMAL + ReceiveToIdle，IDLE 回调后需要任务重新 arm DMA。
     * 保留明确的帧间隔，避免下一帧落入接收端的 rearm 空窗。
     */
    esp_rom_delay_us(PWOS_COORDINATOR_TX_GUARD_US);
    runtime->stats.tx_bytes += (uint32_t)frame_len;
    ++runtime->stats.tx_frames;
    runtime->stats.last_tx_tick = (uint32_t)xTaskGetTickCount();
    (void)xSemaphoreGive(runtime->tx_mutex);
    return 0;
}

static int send_hello(pwos_coordinator_runtime_t *runtime, uint8_t type)
{
    uint8_t payload[PWOS_COORDINATOR_HELLO_PAYLOAD_LEN];

    encode_hello_payload(runtime, 0u, payload);
    if (send_payload(
            runtime,
            type,
            PWOS_LINK_ADDR_UNASSIGNED,
            1u,
            payload,
            (uint16_t)sizeof(payload)) != 0) {
        return -1;
    }

    if (type == (uint8_t)PWOS_LINK_TYPE_LINK_HELLO) {
        ++runtime->stats.hello_tx;
    } else if (type == (uint8_t)PWOS_LINK_TYPE_LINK_HELLO_ACK) {
        ++runtime->stats.hello_ack_tx;
    }
    return 0;
}

static int send_mesh2_payload(
    pwos_coordinator_runtime_t *runtime,
    uint8_t type,
    uint8_t dst,
    const uint8_t *payload,
    uint16_t payload_len)
{
    return send_payload(
        runtime,
        type,
        dst,
        PWOS_COORDINATOR_DEFAULT_TTL,
        payload,
        payload_len);
}

static int send_host_advertise(pwos_coordinator_runtime_t *runtime)
{
    pwos_host_rpc_runtime_status_t host;
    pwos_mesh2_host_advertise_t advertise;
    uint8_t payload[PWOS_MESH2_HOST_ADVERTISE_PAYLOAD_LEN];
    size_t payload_len = 0u;

    memset(&host, 0, sizeof(host));
    pwos_host_rpc_runtime_get_status(&host);
    if (host.initialized == 0u) {
        return 0;
    }
    memset(&advertise, 0, sizeof(advertise));
    memcpy(advertise.host_uid, host.local_uid, sizeof(advertise.host_uid));
    advertise.epoch = host.local_epoch;
    advertise.cluster_id = 0x50574F53u; /* "PWOS" */
    advertise.priority = host.local_priority;
    advertise.role = host.local_role;
    if (pwos_mesh2_encode_host_advertise(
            &advertise,
            payload,
            sizeof(payload),
            &payload_len) != PWOS_OK ||
        send_mesh2_payload(
            runtime,
            (uint8_t)PWOS_LINK_TYPE_CTRL_HOST_ADVERTISE,
            PWOS_LINK_ADDR_UNASSIGNED,
            payload,
            (uint16_t)payload_len) != 0) {
        return -1;
    }
    ++runtime->stats.host_advertise_tx;
    return 0;
}

static int handle_node_register(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view)
{
    pwos_mesh2_node_register_t reg;
    pwos_mesh2_addr_assign_t assign;
    uint8_t payload[PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN];
    uint8_t restarted = 0u;
    uint8_t old_addr = 0u;
    uint32_t old_boot_id = 0u;
    size_t payload_len = 0u;

    if (pwos_mesh2_decode_node_register(view->payload, view->payload_len, &reg) != PWOS_OK) {
        ++runtime->stats.rx_parse_errors;
        return -1;
    }
    coordinator_lock(runtime);
    {
        const pwos_host_node_entry_t *old =
            pwos_host_coordinator_find_by_uid(&runtime->coordinator, reg.uid);

        if (old != NULL && old->boot_id != reg.boot_id) {
            restarted = 1u;
            old_addr = old->addr;
            old_boot_id = old->boot_id;
        }
    }
    if (pwos_host_coordinator_handle_register(&runtime->coordinator, &reg, &assign) != 0) {
        coordinator_unlock(runtime);
        ++runtime->stats.tx_errors;
        return -1;
    }
    runtime->stats.node_count = count_nodes(&runtime->coordinator);
    coordinator_unlock(runtime);
    if (restarted != 0u) {
        (void)pwos_job_manager_mark_node_lost(
            &runtime->jobs,
            old_addr,
            old_boot_id,
            PWOS_SESSION_ERR_STALE_BOOT);
    }
    if (pwos_mesh2_encode_addr_assign(
            &assign, payload, sizeof(payload), &payload_len) != PWOS_OK) {
        ++runtime->stats.tx_errors;
        return -1;
    }
    if (send_mesh2_payload(
            runtime,
            (uint8_t)PWOS_LINK_TYPE_CTRL_ADDR_ASSIGN,
            PWOS_LINK_ADDR_UNASSIGNED,
            payload,
            (uint16_t)payload_len) != 0) {
        return -1;
    }

    ++runtime->stats.register_rx;
    ++runtime->stats.assign_tx;
    sync_cluster_vfs(runtime);
    ESP_LOGI(TAG,
        "assign addr=%u uid=%08" PRIx32 "-%08" PRIx32 "-%08" PRIx32
        " boot=%" PRIu32 " upstream_port=%u",
        assign.addr,
        assign.uid[0],
        assign.uid[1],
        assign.uid[2],
        assign.boot_id,
        reg.upstream_port);
    return 0;
}

static int handle_lease_renew(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view)
{
    pwos_mesh2_lease_renew_t renew;
    pwos_mesh2_lease_ack_t ack;
    uint8_t payload[PWOS_MESH2_LEASE_ACK_PAYLOAD_LEN];
    size_t payload_len = 0u;

    if (pwos_mesh2_decode_lease_renew(view->payload, view->payload_len, &renew) != PWOS_OK) {
        ++runtime->stats.rx_parse_errors;
        return -1;
    }
    coordinator_lock(runtime);
    if (pwos_host_coordinator_handle_lease_renew(&runtime->coordinator, &renew, &ack) != 0) {
        coordinator_unlock(runtime);
        ESP_LOGW(TAG,
            "lease renew rejected addr=%u uid=%08" PRIx32 "-%08" PRIx32 "-%08" PRIx32,
            renew.addr,
            renew.uid[0],
            renew.uid[1],
            renew.uid[2]);
        return -1;
    }
    coordinator_unlock(runtime);
    if (pwos_mesh2_encode_lease_ack(&ack, payload, sizeof(payload), &payload_len) != PWOS_OK) {
        ++runtime->stats.tx_errors;
        return -1;
    }
    if (send_mesh2_payload(
            runtime,
            (uint8_t)PWOS_LINK_TYPE_CTRL_LEASE_ACK,
            ack.addr,
            payload,
            (uint16_t)payload_len) != 0) {
        return -1;
    }

    ++runtime->stats.lease_renew_rx;
    ++runtime->stats.lease_ack_tx;
    return 0;
}

static int handle_link_state(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view)
{
    pwos_mesh2_link_state_t link;
    pwos_mesh2_route_update_t route;
    pwos_mesh2_route_update_t reverse_route;
    uint8_t owner_addr = 0u;
    uint8_t reverse_owner = 0u;
    uint8_t payload[PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int rc;

    if (pwos_mesh2_decode_link_state(view->payload, view->payload_len, &link) != PWOS_OK) {
        ++runtime->stats.rx_parse_errors;
        return -1;
    }

    coordinator_lock(runtime);
    rc = pwos_host_coordinator_handle_link_state(
        &runtime->coordinator,
        &link,
        &route,
        &owner_addr,
        &reverse_route,
        &reverse_owner);
    coordinator_unlock(runtime);
    if (rc <= 0) {
        ++runtime->stats.link_state_rx;
        return rc;
    }

    /* 正向路由：发给 local（上报链路的节点）。 */
    if (pwos_mesh2_encode_route_update(&route, payload, sizeof(payload), &payload_len) != PWOS_OK) {
        ++runtime->stats.tx_errors;
        return -1;
    }
    if (send_mesh2_payload(
            runtime,
            (uint8_t)PWOS_LINK_TYPE_CTRL_ROUTE_UPDATE,
            owner_addr,
            payload,
            (uint16_t)payload_len) != 0) {
        return -1;
    }

    ++runtime->stats.link_state_rx;
    ++runtime->stats.route_update_tx;
    ESP_LOGI(TAG,
        "route owner=%u dst=%u next=%u metric=%u action=%u version=%" PRIu32,
        owner_addr,
        route.dst,
        route.next_hop,
        route.metric,
        route.action,
        route.route_version);

    /* 反向路由：发给 peer（链路对端节点），使其也知道怎么到 local。 */
    if (reverse_owner != 0u && reverse_route.action != 0u) {
        payload_len = 0u;
        if (pwos_mesh2_encode_route_update(&reverse_route, payload, sizeof(payload), &payload_len) != PWOS_OK) {
            ++runtime->stats.tx_errors;
        } else if (send_mesh2_payload(
                runtime,
                (uint8_t)PWOS_LINK_TYPE_CTRL_ROUTE_UPDATE,
                reverse_owner,
                payload,
                (uint16_t)payload_len) != 0) {
            ++runtime->stats.tx_errors;
        } else {
            ++runtime->stats.route_update_tx;
            ESP_LOGI(TAG,
                "route owner=%u dst=%u next=%u metric=%u action=%u version=%" PRIu32,
                reverse_owner,
                reverse_route.dst,
                reverse_route.next_hop,
                reverse_route.metric,
                reverse_route.action,
                reverse_route.route_version);
        }
    }
    return 0;
}

static int handle_time_sync(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view)
{
    pwos_mesh2_time_sync_t exchange;
    uint8_t payload[PWOS_MESH2_TIME_SYNC_PAYLOAD_LEN];
    uint64_t server_rx_us = 0u;
    size_t payload_len = 0u;

    ++runtime->stats.time_sync_rx;
    if (view->dst != PWOS_LINK_ADDR_HOST ||
        pwos_host_rpc_runtime_wall_time_us(&server_rx_us) != 0 ||
        pwos_mesh2_decode_time_sync(
            view->payload, view->payload_len, &exchange) != PWOS_OK ||
        exchange.kind != PWOS_MESH2_TIME_SYNC_REQUEST) {
        ++runtime->stats.time_sync_unavailable;
        return -1;
    }

    exchange.kind = PWOS_MESH2_TIME_SYNC_RESPONSE;
    exchange.flags = PWOS_MESH2_TIME_SYNC_FLAG_WALL_VALID;
    exchange.server_rx_unix_us = server_rx_us;
    if (pwos_host_rpc_runtime_wall_time_us(
            &exchange.server_tx_unix_us) != 0 ||
        pwos_mesh2_encode_time_sync(
            &exchange, payload, sizeof(payload), &payload_len) != PWOS_OK ||
        send_mesh2_payload(
            runtime,
            (uint8_t)PWOS_LINK_TYPE_CTRL_TIME_SYNC,
            view->src,
            payload,
            (uint16_t)payload_len) != 0) {
        ++runtime->stats.time_sync_unavailable;
        return -1;
    }
    ++runtime->stats.time_sync_tx;
    return 0;
}

static void handle_frame(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view)
{
    int rc;

    ++runtime->stats.rx_frames;
    runtime->stats.last_rx_tick = (uint32_t)xTaskGetTickCount();

    if (runtime->control_leader == 0u &&
        view->type != PWOS_LINK_TYPE_LINK_HELLO &&
        view->type != PWOS_LINK_TYPE_LINK_HELLO_ACK) {
        ++runtime->stats.nonleader_rx_drop;
        return;
    }

    switch (view->type) {
    case PWOS_LINK_TYPE_LINK_HELLO:
        ++runtime->stats.hello_rx;
        (void)send_hello(runtime, (uint8_t)PWOS_LINK_TYPE_LINK_HELLO_ACK);
        break;
    case PWOS_LINK_TYPE_LINK_HELLO_ACK:
        ++runtime->stats.hello_ack_rx;
        break;
    case PWOS_LINK_TYPE_CTRL_NODE_REGISTER:
        (void)handle_node_register(runtime, view);
        break;
    case PWOS_LINK_TYPE_CTRL_LEASE_RENEW:
        (void)handle_lease_renew(runtime, view);
        break;
    case PWOS_LINK_TYPE_CTRL_LINK_STATE:
        (void)handle_link_state(runtime, view);
        break;
    case PWOS_LINK_TYPE_CTRL_TIME_SYNC:
        (void)handle_time_sync(runtime, view);
        break;
    case PWOS_LINK_TYPE_DATA_MINI9P:
        ++runtime->stats.data_rx;
        if (view->dst != PWOS_LINK_ADDR_HOST) {
            break;
        }
        rc = pwos_session_manager_deliver_mini9p(
            &runtime->sessions,
            view->src,
            view->payload,
            view->payload_len);
        if (rc == 1) {
            ++runtime->stats.mini9p_rx;
        }
        break;
    case PWOS_LINK_TYPE_DATA_RPC:
    {
        pwos_rpc_frame_view_t rpc_view;

        ++runtime->stats.data_rx;
        if (view->dst != PWOS_LINK_ADDR_HOST) {
            break;
        }
        if (pwos_rpc_decode(view->payload, view->payload_len, &rpc_view) != 0 ||
            (rpc_view.kind != PWOS_RPC_KIND_RESPONSE &&
             rpc_view.kind != PWOS_RPC_KIND_STREAM_CHUNK &&
             rpc_view.kind != PWOS_RPC_KIND_STREAM_END)) {
            ++runtime->stats.rpc_malformed;
            break;
        }
        if (rpc_view.kind == PWOS_RPC_KIND_RESPONSE) {
            rc = pwos_session_manager_deliver_data(
                &runtime->sessions,
                (uint8_t)PWOS_LINK_TYPE_DATA_RPC,
                view->src,
                rpc_view.call_id,
                view->payload,
                view->payload_len);
        } else {
            rc = pwos_session_manager_deliver_data_part(
                &runtime->sessions,
                (uint8_t)PWOS_LINK_TYPE_DATA_RPC,
                view->src,
                rpc_view.call_id,
                rpc_view.payload,
                rpc_view.payload_len,
                rpc_view.kind == PWOS_RPC_KIND_STREAM_END ? 1u : 0u,
                rpc_view.status);
        }
        if (rc == 1) {
            ++runtime->stats.rpc_rx;
        }
        break;
    }
    case PWOS_LINK_TYPE_DATA_JOB:
    {
        pwos_job_frame_view_t job_view;

        ++runtime->stats.data_rx;
        if (view->dst != PWOS_LINK_ADDR_HOST) {
            break;
        }
        if (pwos_job_decode(view->payload, view->payload_len, &job_view) != 0 ||
            (job_view.kind != PWOS_JOB_KIND_CAPS_RESPONSE &&
             job_view.kind != PWOS_JOB_KIND_SUBMIT_ACK &&
             job_view.kind != PWOS_JOB_KIND_STATUS_RESPONSE &&
             job_view.kind != PWOS_JOB_KIND_RESULT_RESPONSE &&
             job_view.kind != PWOS_JOB_KIND_CANCEL_ACK)) {
            ++runtime->stats.job_malformed;
            break;
        }
        rc = pwos_session_manager_deliver_data(
            &runtime->sessions,
            (uint8_t)PWOS_LINK_TYPE_DATA_JOB,
            view->src,
            job_view.request_id,
            view->payload,
            view->payload_len);
        if (rc == 1) {
            ++runtime->stats.job_rx;
        }
        break;
    }
    default:
        if (pwos_link_type_is_data(view->type)) {
            ++runtime->stats.data_rx;
        }
        break;
    }
}

static void consume_rx_bytes(
    pwos_coordinator_runtime_t *runtime,
    const uint8_t *data,
    size_t len)
{
    size_t offset = 0u;

    runtime->stats.rx_bytes += (uint32_t)len;
    while (offset < len) {
        pwos_link_parse_event_t event;
        size_t consumed = 0u;
        pwos_link_parse_kind_t kind;

        kind = pwos_link_parser_feed(
            &runtime->parser,
            data + offset,
            len - offset,
            &event,
            &consumed);
        if (consumed == 0u) {
            break;
        }
        offset += consumed;

        if (kind == PWOS_LINK_PARSE_FRAME) {
            handle_frame(runtime, &event.frame);
        } else if (kind == PWOS_LINK_PARSE_ERROR) {
            ++runtime->stats.rx_parse_errors;
        }
    }
}

static int session_send_data(
    void *ctx,
    uint8_t data_type,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;
    int rc;

    if (runtime == NULL || payload == NULL || payload_len == 0u ||
        payload_len > PWOS_LINK_MAX_PAYLOAD_LEN ||
        !pwos_link_type_is_data(data_type)) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = send_mesh2_payload(
            runtime,
            data_type,
            dst_addr,
            payload,
            payload_len);
    if (rc != 0) {
        if (rc == PWOS_SESSION_ERR_QUEUE_FULL) {
            return rc;
        }
        return -(int)M9P_ERR_EIO;
    }
    if (data_type == (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P) {
        ++runtime->stats.mini9p_tx;
    } else if (data_type == (uint8_t)PWOS_LINK_TYPE_DATA_RPC) {
        ++runtime->stats.rpc_tx;
    } else if (data_type == (uint8_t)PWOS_LINK_TYPE_DATA_JOB) {
        ++runtime->stats.job_tx;
    }
    return 0;
}

static int run_cluster_health_probe(
    pwos_coordinator_runtime_t *runtime,
    const pwos_cluster_vfs_route_t *route)
{
    char path[48];
    uint8_t text[PWOS_COORDINATOR_MINI9P_HEALTH_READ_LEN + 1u];
    uint16_t count = PWOS_COORDINATOR_MINI9P_HEALTH_READ_LEN;
    int rc;
    int written;

    if (runtime == NULL || route == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    written = snprintf(path, sizeof(path), "/%s/sys/health", route->target);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return -(int)M9P_ERR_EMSIZE;
    }

    rc = pwos_cluster_vfs_read_path(
        &runtime->cluster_vfs,
        path,
        text,
        &count,
        PWOS_COORDINATOR_MINI9P_DEADLINE_MS);
    if (rc == 0) {
        uint16_t i;

        if (count > PWOS_COORDINATOR_MINI9P_HEALTH_READ_LEN) {
            count = PWOS_COORDINATOR_MINI9P_HEALTH_READ_LEN;
        }
        /* /sys/health 可包含多行诊断，周期日志只保留状态首行。 */
        for (i = 0u; i < count; ++i) {
            if (text[i] == (uint8_t)'\r' || text[i] == (uint8_t)'\n') {
                count = i;
                break;
            }
        }
        text[count] = '\0';
        ESP_LOGI(TAG,
            "mini9p %s addr=%u /sys/health=%s",
            route->target,
            route->addr,
            (const char *)text);
    }
    return rc;
}

static void probe_one_due_node(pwos_coordinator_runtime_t *runtime)
{
    TickType_t now;
    size_t i;

    if (runtime == NULL || runtime->control_leader == 0u) {
        return;
    }

    now = xTaskGetTickCount();
    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        pwos_cluster_vfs_route_t route;
        int rc;

        if (pwos_cluster_vfs_get_route(&runtime->cluster_vfs, i, &route) != 0 ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE) {
            continue;
        }
        if (runtime->mini9p_last_probe_tick[i] != 0u &&
            (uint32_t)(now - runtime->mini9p_last_probe_tick[i]) <
                pdMS_TO_TICKS(PWOS_COORDINATOR_MINI9P_PROBE_INTERVAL_MS)) {
            continue;
        }

        runtime->mini9p_last_probe_tick[i] = (uint32_t)now;
        runtime->stats.mini9p_last_addr = route.addr;
        rc = run_cluster_health_probe(runtime, &route);
        runtime->stats.mini9p_last_error = (int32_t)rc;
        if (rc == 0) {
            ++runtime->stats.mini9p_probe_ok;
        } else {
            ++runtime->stats.mini9p_probe_fail;
            ESP_LOGW(TAG,
                "mini9p probe %s addr=%u failed rc=%d (%s)",
                route.target,
                route.addr,
                rc,
                pwos_session_error_name(rc));
        }
        return;
    }
}

static void log_status(pwos_coordinator_runtime_t *runtime)
{
    pwos_session_manager_stats_t session_stats;
    pwos_job_manager_stats_t job_stats;

    pwos_session_manager_get_stats(&runtime->sessions, &session_stats);
    pwos_job_manager_get_stats(&runtime->jobs, &job_stats);
    ESP_LOGI(TAG,
        "stats nodes=%u rx=%" PRIu32 "/%" PRIu32
        " tx=%" PRIu32 "/%" PRIu32
        " hello=%" PRIu32 "/%" PRIu32
        " reg=%" PRIu32 " assign=%" PRIu32
        " renew=%" PRIu32 " route=%" PRIu32
        " host_adv=%" PRIu32 " leader=%u follower_drop=%" PRIu32
        " m9p=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32
        " rpc=%" PRIu32 "/%" PRIu32 "/%" PRIu32
        " job=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " lost=%" PRIu32
        " pending=%" PRIu32 "/%" PRIu32 " peak=%" PRIu32
        " parse_err=%" PRIu32 " tx_err=%" PRIu32,
        runtime->stats.node_count,
        runtime->stats.rx_frames,
        runtime->stats.rx_bytes,
        runtime->stats.tx_frames,
        runtime->stats.tx_bytes,
        runtime->stats.hello_rx,
        runtime->stats.hello_ack_tx,
        runtime->stats.register_rx,
        runtime->stats.assign_tx,
        runtime->stats.lease_renew_rx,
        runtime->stats.route_update_tx,
        runtime->stats.host_advertise_tx,
        runtime->stats.control_leader,
        runtime->stats.nonleader_rx_drop,
        runtime->stats.mini9p_probe_ok,
        runtime->stats.mini9p_probe_fail,
        runtime->stats.mini9p_tx,
        runtime->stats.mini9p_rx,
        runtime->stats.rpc_tx,
        runtime->stats.rpc_rx,
        runtime->stats.rpc_malformed,
        runtime->stats.job_tx,
        runtime->stats.job_rx,
        runtime->stats.job_malformed,
        job_stats.lost,
        session_stats.pending_delivered,
        session_stats.pending_unmatched,
        session_stats.pending_peak,
        runtime->stats.rx_parse_errors,
        runtime->stats.tx_errors);
}

static void session_manager_lock(void *ctx)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    (void)xSemaphoreTake(runtime->session_mutex, portMAX_DELAY);
}

static void session_manager_unlock(void *ctx)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    (void)xSemaphoreGive(runtime->session_mutex);
}

static void session_client_lock(void *ctx, uint8_t session_index)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    if (session_index < PWOS_SESSION_MANAGER_MAX_SESSIONS) {
        (void)xSemaphoreTake(runtime->session_client_mutex[session_index], portMAX_DELAY);
    }
}

static void session_client_unlock(void *ctx, uint8_t session_index)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    if (session_index < PWOS_SESSION_MANAGER_MAX_SESSIONS) {
        (void)xSemaphoreGive(runtime->session_client_mutex[session_index]);
    }
}

static void session_pending_reset(void *ctx, uint8_t pending_index)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    if (pending_index < PWOS_SESSION_MANAGER_MAX_PENDING) {
        while (xSemaphoreTake(runtime->pending_event[pending_index], 0u) == pdTRUE) {
            /* 清掉上一次事务可能遗留的 signal。 */
        }
    }
}

static int session_pending_wait(
    void *ctx,
    uint8_t pending_index,
    uint32_t timeout_ms)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;
    TickType_t timeout_ticks;

    if (pending_index >= PWOS_SESSION_MANAGER_MAX_PENDING) {
        return -(int)M9P_ERR_EINVAL;
    }
    timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0u && timeout_ticks == 0u) {
        timeout_ticks = 1u;
    }
    return xSemaphoreTake(runtime->pending_event[pending_index], timeout_ticks) == pdTRUE ?
        0 : PWOS_SESSION_ERR_DEADLINE;
}

static void session_pending_signal(void *ctx, uint8_t pending_index)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    if (pending_index < PWOS_SESSION_MANAGER_MAX_PENDING) {
        (void)xSemaphoreGive(runtime->pending_event[pending_index]);
    }
}

static uint32_t session_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void cluster_vfs_lock(void *ctx)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    (void)xSemaphoreTake(runtime->vfs_mutex, portMAX_DELAY);
}

static void cluster_vfs_unlock(void *ctx)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    (void)xSemaphoreGive(runtime->vfs_mutex);
}

static void job_manager_lock(void *ctx)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    (void)xSemaphoreTake(runtime->job_mutex, portMAX_DELAY);
}

static void job_manager_unlock(void *ctx)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    (void)xSemaphoreGive(runtime->job_mutex);
}

static int resolve_job_target(
    void *ctx,
    const char *target,
    uint8_t *out_addr,
    uint32_t *out_boot_id)
{
    pwos_cluster_vfs_route_t route;
    int rc;

    (void)ctx;
    if (out_addr == NULL || out_boot_id == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    rc = find_rpc_route(target, &route);
    if (rc != 0) {
        return rc;
    }
    *out_addr = route.addr;
    *out_boot_id = route.boot_id;
    return 0;
}

static void delete_runtime_sync(pwos_coordinator_runtime_t *runtime)
{
    size_t i;

    if (runtime == NULL) {
        return;
    }
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        if (runtime->pending_event[i] != NULL) {
            vSemaphoreDelete(runtime->pending_event[i]);
            runtime->pending_event[i] = NULL;
        }
    }
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        if (runtime->session_client_mutex[i] != NULL) {
            vSemaphoreDelete(runtime->session_client_mutex[i]);
            runtime->session_client_mutex[i] = NULL;
        }
    }
    if (runtime->vfs_mutex != NULL) {
        vSemaphoreDelete(runtime->vfs_mutex);
        runtime->vfs_mutex = NULL;
    }
    if (runtime->job_mutex != NULL) {
        vSemaphoreDelete(runtime->job_mutex);
        runtime->job_mutex = NULL;
    }
    if (runtime->session_mutex != NULL) {
        vSemaphoreDelete(runtime->session_mutex);
        runtime->session_mutex = NULL;
    }
    if (runtime->coordinator_mutex != NULL) {
        vSemaphoreDelete(runtime->coordinator_mutex);
        runtime->coordinator_mutex = NULL;
    }
    if (runtime->tx_mutex != NULL) {
        vSemaphoreDelete(runtime->tx_mutex);
        runtime->tx_mutex = NULL;
    }
}

static int init_runtime_sync(pwos_coordinator_runtime_t *runtime)
{
    size_t i;

    runtime->tx_mutex = xSemaphoreCreateMutex();
    runtime->coordinator_mutex = xSemaphoreCreateMutex();
    runtime->session_mutex = xSemaphoreCreateMutex();
    runtime->vfs_mutex = xSemaphoreCreateMutex();
    runtime->job_mutex = xSemaphoreCreateMutex();
    if (runtime->tx_mutex == NULL || runtime->coordinator_mutex == NULL ||
        runtime->session_mutex == NULL ||
        runtime->vfs_mutex == NULL || runtime->job_mutex == NULL) {
        delete_runtime_sync(runtime);
        return -1;
    }

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        runtime->session_client_mutex[i] = xSemaphoreCreateMutex();
        if (runtime->session_client_mutex[i] == NULL) {
            delete_runtime_sync(runtime);
            return -1;
        }
    }
    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_PENDING; ++i) {
        runtime->pending_event[i] = xSemaphoreCreateBinary();
        if (runtime->pending_event[i] == NULL) {
            delete_runtime_sync(runtime);
            return -1;
        }
    }
    return 0;
}

static void coordinator_task(void *arg)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)arg;
    uint8_t rx_buf[PWOS_COORDINATOR_READ_SLICE_SIZE];
    TickType_t last_hello = 0u;
    TickType_t last_report = 0u;
    TickType_t last_host_advertise = 0u;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        int rx_len;

        reconcile_control_role(runtime);

        if (last_hello == 0u ||
            (uint32_t)(now - last_hello) >= pdMS_TO_TICKS(PWOS_COORDINATOR_HELLO_INTERVAL_MS)) {
            (void)send_hello(runtime, (uint8_t)PWOS_LINK_TYPE_LINK_HELLO);
            last_hello = now;
        }

        if (last_host_advertise == 0u ||
            (uint32_t)(now - last_host_advertise) >=
                pdMS_TO_TICKS(PWOS_COORDINATOR_HOST_ADVERTISE_INTERVAL_MS)) {
            (void)send_host_advertise(runtime);
            last_host_advertise = now;
        }

        rx_len = uart_read_bytes(
            (uart_port_t)runtime->uart_port,
            rx_buf,
            sizeof(rx_buf),
            pdMS_TO_TICKS(20));
        if (rx_len > 0) {
            consume_rx_bytes(runtime, rx_buf, (size_t)rx_len);
        }

        if (last_report == 0u ||
            (uint32_t)(now - last_report) >= pdMS_TO_TICKS(PWOS_COORDINATOR_REPORT_INTERVAL_MS)) {
            coordinator_lock(runtime);
            runtime->stats.node_count = count_nodes(&runtime->coordinator);
            coordinator_unlock(runtime);
            log_status(runtime);
            last_report = now;
        }
    }
}

static void mini9p_probe_task(void *arg)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)arg;

    for (;;) {
        probe_one_due_node(runtime);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static int init_uart(void)
{
    uart_config_t uart_config;
    esp_err_t err;

    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.baud_rate = PWOS_COORDINATOR_UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    err = uart_param_config((uart_port_t)PWOS_COORDINATOR_UART_PORT, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = uart_driver_install(
        (uart_port_t)PWOS_COORDINATOR_UART_PORT,
        PWOS_COORDINATOR_UART_RX_BUF_SIZE,
        PWOS_COORDINATOR_UART_TX_BUF_SIZE,
        0,
        NULL,
        0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = uart_set_pin(
        (uart_port_t)PWOS_COORDINATOR_UART_PORT,
        PWOS_COORDINATOR_UART_TX_PIN,
        PWOS_COORDINATOR_UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        (void)uart_driver_delete((uart_port_t)PWOS_COORDINATOR_UART_PORT);
        return -1;
    }

    (void)uart_flush_input((uart_port_t)PWOS_COORDINATOR_UART_PORT);
    ESP_LOGI(TAG,
        "uart%d tx=%d rx=%d baud=%d",
        PWOS_COORDINATOR_UART_PORT,
        PWOS_COORDINATOR_UART_TX_PIN,
        PWOS_COORDINATOR_UART_RX_PIN,
        PWOS_COORDINATOR_UART_BAUD_RATE);
    return 0;
}

int pwos_coordinator_runtime_start_default(void)
{
    BaseType_t created;

    if (g_runtime.initialized != 0u) {
        return 0;
    }

    memset(&g_runtime, 0, sizeof(g_runtime));
    g_runtime.uart_port = PWOS_COORDINATOR_UART_PORT;
    g_runtime.boot_id = esp_random();
    g_runtime.uid[0] = 0x5034574Fu; /* "P4WO"，用于 HELLO 中标识 P4 coordinator。 */
    g_runtime.uid[1] = esp_random();
    g_runtime.uid[2] = 0x434F4F52u; /* "COOR" */
    pwos_link_parser_init(&g_runtime.parser);
    pwos_host_coordinator_init(&g_runtime.coordinator);
    if (init_runtime_sync(&g_runtime) != 0) {
        memset(&g_runtime, 0, sizeof(g_runtime));
        return -1;
    }
    {
        pwos_session_manager_config_t session_config;
        pwos_cluster_vfs_config_t vfs_config;
        pwos_job_manager_config_t job_config;
        pwos_job_command_config_t job_command_config;

        memset(&session_config, 0, sizeof(session_config));
        session_config.io_ctx = &g_runtime;
        session_config.send = session_send_data;
        session_config.default_deadline_ms = PWOS_COORDINATOR_MINI9P_DEADLINE_MS;
        session_config.sync_ctx = &g_runtime;
        session_config.lock = session_manager_lock;
        session_config.unlock = session_manager_unlock;
        session_config.client_lock = session_client_lock;
        session_config.client_unlock = session_client_unlock;
        session_config.pending_reset = session_pending_reset;
        session_config.pending_wait = session_pending_wait;
        session_config.pending_signal = session_pending_signal;
        session_config.now_ms = session_now_ms;

        memset(&vfs_config, 0, sizeof(vfs_config));
        vfs_config.lock_ctx = &g_runtime;
        vfs_config.lock = cluster_vfs_lock;
        vfs_config.unlock = cluster_vfs_unlock;
        memset(&job_config, 0, sizeof(job_config));
        job_config.lock_ctx = &g_runtime;
        job_config.lock = job_manager_lock;
        job_config.unlock = job_manager_unlock;
        memset(&job_command_config, 0, sizeof(job_command_config));
        job_command_config.manager = &g_runtime.jobs;
        job_command_config.resolve_ctx = &g_runtime;
        job_command_config.resolve = resolve_job_target;
        if (pwos_session_manager_init(&g_runtime.sessions, &session_config) != 0 ||
            pwos_rpc_client_init(
                &g_runtime.rpc_client, &g_runtime.sessions) != 0 ||
            pwos_job_manager_init(
                &g_runtime.jobs, &g_runtime.sessions, &job_config) != 0 ||
            pwos_job_command_init(
                &g_runtime.job_command, &job_command_config) != 0 ||
            pwos_cluster_vfs_init_with_config(
                &g_runtime.cluster_vfs,
                &g_runtime.sessions,
                &vfs_config) != 0) {
            delete_runtime_sync(&g_runtime);
            memset(&g_runtime, 0, sizeof(g_runtime));
            return -1;
        }
    }

    if (init_uart() != 0) {
        delete_runtime_sync(&g_runtime);
        memset(&g_runtime, 0, sizeof(g_runtime));
        return -1;
    }

    g_runtime.initialized = 1u;
    g_runtime.stats.initialized = 1u;
    g_runtime.stats.uart_port = PWOS_COORDINATOR_UART_PORT;

    created = xTaskCreate(
        coordinator_task,
        PWOS_COORDINATOR_TASK_NAME,
        PWOS_COORDINATOR_TASK_STACK,
        &g_runtime,
        PWOS_COORDINATOR_TASK_PRIO,
        &g_runtime.task);
    if (created != pdPASS) {
        (void)uart_driver_delete((uart_port_t)PWOS_COORDINATOR_UART_PORT);
        delete_runtime_sync(&g_runtime);
        memset(&g_runtime, 0, sizeof(g_runtime));
        return -1;
    }

    created = xTaskCreate(
        mini9p_probe_task,
        PWOS_COORDINATOR_PROBE_TASK_NAME,
        PWOS_COORDINATOR_PROBE_TASK_STACK,
        &g_runtime,
        PWOS_COORDINATOR_PROBE_TASK_PRIO,
        &g_runtime.probe_task);
    if (created != pdPASS) {
        vTaskDelete(g_runtime.task);
        g_runtime.task = NULL;
        (void)uart_driver_delete((uart_port_t)PWOS_COORDINATOR_UART_PORT);
        delete_runtime_sync(&g_runtime);
        memset(&g_runtime, 0, sizeof(g_runtime));
        return -1;
    }

    g_runtime.task_started = 1u;
    g_runtime.stats.task_started = 1u;
    ESP_LOGI(TAG,
        "started boot=%" PRIu32 " uid=%08" PRIx32 "-%08" PRIx32 "-%08" PRIx32,
        g_runtime.boot_id,
        g_runtime.uid[0],
        g_runtime.uid[1],
        g_runtime.uid[2]);
    return 0;
}

#else

int pwos_coordinator_runtime_start_default(void)
{
    return -1;
}

#endif

void pwos_coordinator_runtime_get_stats(pwos_coordinator_runtime_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    *out_stats = g_runtime.stats;
}

int pwos_coordinator_runtime_get_node(
    size_t index,
    pwos_host_node_entry_t *out_node)
{
    if (out_node == NULL || index >= PWOS_HOST_COORDINATOR_MAX_NODES ||
        g_runtime.initialized == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }
#ifdef ESP_PLATFORM
    coordinator_lock(&g_runtime);
#endif
    *out_node = g_runtime.coordinator.nodes[index];
#ifdef ESP_PLATFORM
    coordinator_unlock(&g_runtime);
#endif
    return out_node->valid != 0u ? 0 : -(int)M9P_ERR_ENOENT;
}

int pwos_coordinator_runtime_get_route(
    size_t index,
    pwos_cluster_vfs_route_t *out_route)
{
    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    return pwos_cluster_vfs_get_route(&g_runtime.cluster_vfs, index, out_route);
}

int pwos_coordinator_runtime_get_session(
    size_t index,
    pwos_session_snapshot_t *out_session)
{
    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    return pwos_session_manager_get_snapshot(&g_runtime.sessions, index, out_session);
}

void pwos_coordinator_runtime_get_session_stats(
    pwos_session_manager_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    if (g_runtime.initialized == 0u) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    pwos_session_manager_get_stats(&g_runtime.sessions, out_stats);
}

void pwos_coordinator_runtime_get_vfs_stats(
    pwos_cluster_vfs_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    if (g_runtime.initialized == 0u) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    pwos_cluster_vfs_get_stats(&g_runtime.cluster_vfs, out_stats);
}

void pwos_coordinator_runtime_get_rpc_stats(
    pwos_rpc_client_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    if (g_runtime.initialized == 0u) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    pwos_rpc_client_get_stats(&g_runtime.rpc_client, out_stats);
}

void pwos_coordinator_runtime_get_job_stats(
    pwos_job_manager_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    if (g_runtime.initialized == 0u) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    pwos_job_manager_get_stats(&g_runtime.jobs, out_stats);
}

int pwos_coordinator_runtime_get_job(
    size_t index,
    pwos_job_entry_t *out_job)
{
    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    return pwos_job_manager_get_at(&g_runtime.jobs, index, out_job);
}

int pwos_coordinator_runtime_job_command(
    const char *args,
    char *output,
    size_t output_cap,
    size_t *out_len,
    uint32_t deadline_ms)
{
    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    return pwos_job_command_execute(
        &g_runtime.job_command,
        args,
        output,
        output_cap,
        out_len,
        deadline_ms);
}

int pwos_coordinator_runtime_job_submit(
    const char *target,
    uint8_t kernel,
    const uint8_t *input,
    uint16_t input_len,
    uint32_t deadline_ms,
    uint32_t *out_host_job_id,
    uint16_t *out_remote_status)
{
    pwos_cluster_vfs_route_t route;
    int rc;

    if (g_runtime.initialized == 0u) return PWOS_SESSION_ERR_NO_ROUTE;
    rc = find_rpc_route(target, &route);
    if (rc != 0) return rc;
    return pwos_job_manager_submit(
        &g_runtime.jobs,
        target,
        route.addr,
        route.boot_id,
        kernel,
        input,
        input_len,
        deadline_ms,
        out_host_job_id,
        out_remote_status);
}

int pwos_coordinator_runtime_job_result(
    uint32_t host_job_id,
    uint32_t deadline_ms,
    uint8_t *out_result,
    uint16_t *in_out_len,
    pwos_job_entry_t *out_entry,
    uint16_t *out_remote_status)
{
    if (g_runtime.initialized == 0u) return PWOS_SESSION_ERR_NO_ROUTE;
    return pwos_job_manager_result(
        &g_runtime.jobs,
        host_job_id,
        deadline_ms,
        out_result,
        in_out_len,
        out_entry,
        out_remote_status);
}

static int find_rpc_route(
    const char *target,
    pwos_cluster_vfs_route_t *out_route)
{
    size_t i;

    if (target == NULL || out_route == NULL || target[0] == '\0') {
        return -(int)M9P_ERR_EINVAL;
    }
    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        pwos_cluster_vfs_route_t route;

        if (pwos_cluster_vfs_get_route(&g_runtime.cluster_vfs, i, &route) != 0 ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE ||
            strcmp(route.target, target) != 0) {
            continue;
        }
        *out_route = route;
        return 0;
    }
    return PWOS_SESSION_ERR_NO_ROUTE;
}

int pwos_coordinator_runtime_rpc_call(
    const char *target,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status)
{
    pwos_cluster_vfs_route_t route;
    int rc;

    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    rc = find_rpc_route(target, &route);
    if (rc != 0) {
        return rc;
    }
    return pwos_rpc_client_call(
        &g_runtime.rpc_client,
        route.addr,
        route.boot_id,
        service,
        method,
        payload,
        payload_len,
        deadline_ms,
        response,
        in_out_response_len,
        out_status);
}

int pwos_coordinator_runtime_rpc_notify(
    const char *target,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms)
{
    pwos_cluster_vfs_route_t route;
    int rc;

    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    rc = find_rpc_route(target, &route);
    if (rc != 0) {
        return rc;
    }
    return pwos_rpc_client_notify(
        &g_runtime.rpc_client,
        route.addr,
        route.boot_id,
        service,
        method,
        payload,
        payload_len,
        deadline_ms);
}

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
    uint16_t *out_chunk_count)
{
    pwos_cluster_vfs_route_t route;
    int rc;

    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    rc = find_rpc_route(target, &route);
    if (rc != 0) {
        return rc;
    }
    return pwos_rpc_client_stream(
        &g_runtime.rpc_client,
        route.addr,
        route.boot_id,
        service,
        method,
        payload,
        payload_len,
        deadline_ms,
        response,
        in_out_response_len,
        out_status,
        out_chunk_count);
}

int pwos_coordinator_runtime_read_path(
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    return pwos_cluster_vfs_read_path(
        &g_runtime.cluster_vfs,
        path,
        buf,
        in_out_len,
        deadline_ms);
}

int pwos_coordinator_runtime_write_path(
    const char *path,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    return pwos_cluster_vfs_write_path(
        &g_runtime.cluster_vfs,
        path,
        data,
        len,
        out_written,
        deadline_ms);
}

int pwos_coordinator_runtime_list(
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t deadline_ms)
{
    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    return pwos_cluster_vfs_list(
        &g_runtime.cluster_vfs,
        path,
        entries,
        max_entries,
        out_count,
        deadline_ms);
}

int pwos_coordinator_runtime_stat(
    const char *path,
    struct m9p_stat *out_stat,
    uint32_t deadline_ms)
{
    if (g_runtime.initialized == 0u) {
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    return pwos_cluster_vfs_stat(
        &g_runtime.cluster_vfs,
        path,
        out_stat,
        deadline_ms);
}
