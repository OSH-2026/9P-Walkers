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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "cluster_vfs.h"
#include "pwos_link_frame.h"
#include "pwos_link_parser.h"
#include "pwos_mesh2_control.h"
#include "session_manager.h"

#define PWOS_COORDINATOR_TASK_NAME "pwos_coord"
#define PWOS_COORDINATOR_TASK_STACK 4096
#define PWOS_COORDINATOR_TASK_PRIO 8
#define PWOS_COORDINATOR_UART_RX_BUF_SIZE 2048
#define PWOS_COORDINATOR_UART_TX_BUF_SIZE 2048
#define PWOS_COORDINATOR_READ_SLICE_SIZE 128
#define PWOS_COORDINATOR_HELLO_INTERVAL_MS 500u
#define PWOS_COORDINATOR_REPORT_INTERVAL_MS 5000u
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
    uint16_t next_seq;
    uint32_t boot_id;
    uint32_t uid[3];
    pwos_link_parser_t parser;
    pwos_host_coordinator_t coordinator;
    pwos_session_manager_t sessions;
    pwos_cluster_vfs_t cluster_vfs;
    uint32_t mini9p_last_probe_tick[PWOS_HOST_COORDINATOR_MAX_NODES];
    pwos_coordinator_runtime_stats_t stats;
#ifdef ESP_PLATFORM
    TaskHandle_t task;
#endif
} pwos_coordinator_runtime_t;

typedef struct {
    pwos_coordinator_runtime_t *runtime;
    uint8_t src_addr;
    uint8_t captured;
    uint8_t *rx_data;
    size_t rx_cap;
    size_t *rx_len;
} pwos_mini9p_capture_t;

static pwos_coordinator_runtime_t g_runtime;

#ifdef ESP_PLATFORM
static const char *TAG = "pwos_coord";

static void consume_rx_bytes_with_capture(
    pwos_coordinator_runtime_t *runtime,
    const uint8_t *data,
    size_t len,
    pwos_mini9p_capture_t *capture);

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
    int rc;

    if (runtime == NULL) {
        return;
    }

    rc = pwos_cluster_vfs_sync_from_coordinator(
        &runtime->cluster_vfs,
        &runtime->coordinator);
    if (rc != 0) {
        runtime->stats.mini9p_last_error = (int32_t)rc;
        ESP_LOGW(TAG, "cluster_vfs sync failed rc=%d (%s)", rc, pwos_session_error_name(rc));
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
        return -1;
    }

    written = uart_write_bytes((uart_port_t)runtime->uart_port, frame, frame_len);
    if (written < 0 || (size_t)written != frame_len) {
        ++runtime->stats.tx_errors;
        ESP_LOGW(TAG, "uart write type=0x%02x failed written=%d len=%u",
            type,
            written,
            (unsigned)frame_len);
        return -1;
    }

    (void)uart_wait_tx_done((uart_port_t)runtime->uart_port, pdMS_TO_TICKS(100));
    runtime->stats.tx_bytes += (uint32_t)frame_len;
    ++runtime->stats.tx_frames;
    runtime->stats.last_tx_tick = (uint32_t)xTaskGetTickCount();
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

static int handle_node_register(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view)
{
    pwos_mesh2_node_register_t reg;
    pwos_mesh2_addr_assign_t assign;
    uint8_t payload[PWOS_MESH2_ADDR_ASSIGN_PAYLOAD_LEN];
    size_t payload_len = 0u;

    if (pwos_mesh2_decode_node_register(view->payload, view->payload_len, &reg) != PWOS_OK) {
        ++runtime->stats.rx_parse_errors;
        return -1;
    }
    if (pwos_host_coordinator_handle_register(&runtime->coordinator, &reg, &assign) != 0) {
        ++runtime->stats.tx_errors;
        return -1;
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
    runtime->stats.node_count = count_nodes(&runtime->coordinator);
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
    if (pwos_host_coordinator_handle_lease_renew(&runtime->coordinator, &renew, &ack) != 0) {
        ESP_LOGW(TAG,
            "lease renew rejected addr=%u uid=%08" PRIx32 "-%08" PRIx32 "-%08" PRIx32,
            renew.addr,
            renew.uid[0],
            renew.uid[1],
            renew.uid[2]);
        return -1;
    }
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
    uint8_t owner_addr = 0u;
    uint8_t payload[PWOS_MESH2_ROUTE_UPDATE_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int rc;

    if (pwos_mesh2_decode_link_state(view->payload, view->payload_len, &link) != PWOS_OK) {
        ++runtime->stats.rx_parse_errors;
        return -1;
    }

    rc = pwos_host_coordinator_handle_link_state(
        &runtime->coordinator,
        &link,
        &route,
        &owner_addr);
    if (rc <= 0) {
        ++runtime->stats.link_state_rx;
        return rc;
    }

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
    return 0;
}

static uint8_t capture_mini9p_response(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view,
    pwos_mini9p_capture_t *capture)
{
    if (runtime == NULL || view == NULL || capture == NULL ||
        capture->captured != 0u || capture->rx_len == NULL) {
        return 0u;
    }
    if (view->type != (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P ||
        view->src != capture->src_addr ||
        view->dst != PWOS_LINK_ADDR_HOST ||
        view->payload_len > capture->rx_cap ||
        (view->payload_len > 0u && capture->rx_data == NULL)) {
        return 0u;
    }

    if (view->payload_len > 0u) {
        memcpy(capture->rx_data, view->payload, view->payload_len);
    }
    *capture->rx_len = view->payload_len;
    capture->captured = 1u;
    ++runtime->stats.data_rx;
    ++runtime->stats.mini9p_rx;
    return 1u;
}

static void handle_frame_with_capture(
    pwos_coordinator_runtime_t *runtime,
    const pwos_link_frame_view_t *view,
    pwos_mini9p_capture_t *capture)
{
    ++runtime->stats.rx_frames;
    runtime->stats.last_rx_tick = (uint32_t)xTaskGetTickCount();

    if (capture_mini9p_response(runtime, view, capture) != 0u) {
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
    consume_rx_bytes_with_capture(runtime, data, len, NULL);
}

static void consume_rx_bytes_with_capture(
    pwos_coordinator_runtime_t *runtime,
    const uint8_t *data,
    size_t len,
    pwos_mini9p_capture_t *capture)
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
            handle_frame_with_capture(runtime, &event.frame, capture);
        } else if (kind == PWOS_LINK_PARSE_ERROR) {
            ++runtime->stats.rx_parse_errors;
        }
    }
}

static int session_send_mini9p(
    void *ctx,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;

    if (runtime == NULL || payload == NULL || payload_len == 0u ||
        payload_len > PWOS_LINK_MAX_PAYLOAD_LEN) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (send_mesh2_payload(
            runtime,
            (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P,
            dst_addr,
            payload,
            payload_len) != 0) {
        return -(int)M9P_ERR_EIO;
    }
    ++runtime->stats.mini9p_tx;
    return 0;
}

static int session_wait_mini9p(
    void *ctx,
    uint8_t src_addr,
    uint32_t deadline_ms,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_payload_len)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)ctx;
    pwos_mini9p_capture_t capture;
    TickType_t start;
    TickType_t deadline_ticks;
    uint8_t rx_buf[PWOS_COORDINATOR_READ_SLICE_SIZE];

    if (runtime == NULL || out_payload_len == NULL ||
        (payload_cap > 0u && payload == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(&capture, 0, sizeof(capture));
    capture.runtime = runtime;
    capture.src_addr = src_addr;
    capture.rx_data = payload;
    capture.rx_cap = payload_cap;
    capture.rx_len = out_payload_len;
    *out_payload_len = 0u;

    start = xTaskGetTickCount();
    deadline_ticks = pdMS_TO_TICKS(deadline_ms);
    while ((uint32_t)(xTaskGetTickCount() - start) < (uint32_t)deadline_ticks) {
        int rx_count = uart_read_bytes(
            (uart_port_t)runtime->uart_port,
            rx_buf,
            sizeof(rx_buf),
            pdMS_TO_TICKS(20));
        if (rx_count > 0) {
            consume_rx_bytes_with_capture(runtime, rx_buf, (size_t)rx_count, &capture);
            if (capture.captured != 0u) {
                return 0;
            }
        }
    }

    return PWOS_SESSION_ERR_DEADLINE;
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
        if (count > PWOS_COORDINATOR_MINI9P_HEALTH_READ_LEN) {
            count = PWOS_COORDINATOR_MINI9P_HEALTH_READ_LEN;
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

    if (runtime == NULL) {
        return;
    }

    sync_cluster_vfs(runtime);
    now = xTaskGetTickCount();
    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        const pwos_cluster_vfs_route_t *route =
            pwos_cluster_vfs_route_by_index(&runtime->cluster_vfs, i);
        int rc;

        if (route == NULL ||
            route->state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ||
            route->state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE) {
            continue;
        }
        if (runtime->mini9p_last_probe_tick[i] != 0u &&
            (uint32_t)(now - runtime->mini9p_last_probe_tick[i]) <
                pdMS_TO_TICKS(PWOS_COORDINATOR_MINI9P_PROBE_INTERVAL_MS)) {
            continue;
        }

        runtime->mini9p_last_probe_tick[i] = (uint32_t)now;
        runtime->stats.mini9p_last_addr = route->addr;
        rc = run_cluster_health_probe(runtime, route);
        runtime->stats.mini9p_last_error = (int32_t)rc;
        if (rc == 0) {
            ++runtime->stats.mini9p_probe_ok;
        } else {
            ++runtime->stats.mini9p_probe_fail;
            ESP_LOGW(TAG,
                "mini9p probe %s addr=%u failed rc=%d (%s)",
                route->target,
                route->addr,
                rc,
                pwos_session_error_name(rc));
        }
        return;
    }
}

static void log_status(const pwos_coordinator_runtime_t *runtime)
{
    ESP_LOGI(TAG,
        "stats nodes=%u rx=%" PRIu32 "/%" PRIu32
        " tx=%" PRIu32 "/%" PRIu32
        " hello=%" PRIu32 "/%" PRIu32
        " reg=%" PRIu32 " assign=%" PRIu32
        " renew=%" PRIu32 " route=%" PRIu32
        " m9p=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32
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
        runtime->stats.mini9p_probe_ok,
        runtime->stats.mini9p_probe_fail,
        runtime->stats.mini9p_tx,
        runtime->stats.mini9p_rx,
        runtime->stats.rx_parse_errors,
        runtime->stats.tx_errors);
}

static void coordinator_task(void *arg)
{
    pwos_coordinator_runtime_t *runtime = (pwos_coordinator_runtime_t *)arg;
    uint8_t rx_buf[PWOS_COORDINATOR_READ_SLICE_SIZE];
    TickType_t last_hello = 0u;
    TickType_t last_report = 0u;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        int rx_len;

        if (last_hello == 0u ||
            (uint32_t)(now - last_hello) >= pdMS_TO_TICKS(PWOS_COORDINATOR_HELLO_INTERVAL_MS)) {
            (void)send_hello(runtime, (uint8_t)PWOS_LINK_TYPE_LINK_HELLO);
            last_hello = now;
        }

        rx_len = uart_read_bytes(
            (uart_port_t)runtime->uart_port,
            rx_buf,
            sizeof(rx_buf),
            pdMS_TO_TICKS(20));
        if (rx_len > 0) {
            consume_rx_bytes(runtime, rx_buf, (size_t)rx_len);
        }

        probe_one_due_node(runtime);

        if (last_report == 0u ||
            (uint32_t)(now - last_report) >= pdMS_TO_TICKS(PWOS_COORDINATOR_REPORT_INTERVAL_MS)) {
            runtime->stats.node_count = count_nodes(&runtime->coordinator);
            log_status(runtime);
            last_report = now;
        }
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
    {
        pwos_session_manager_config_t session_config;

        memset(&session_config, 0, sizeof(session_config));
        session_config.io_ctx = &g_runtime;
        session_config.send = session_send_mini9p;
        session_config.wait = session_wait_mini9p;
        session_config.default_deadline_ms = PWOS_COORDINATOR_MINI9P_DEADLINE_MS;
        if (pwos_session_manager_init(&g_runtime.sessions, &session_config) != 0 ||
            pwos_cluster_vfs_init(&g_runtime.cluster_vfs, &g_runtime.sessions) != 0) {
            memset(&g_runtime, 0, sizeof(g_runtime));
            return -1;
        }
    }

    if (init_uart() != 0) {
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

const pwos_host_coordinator_t *pwos_coordinator_runtime_get_coordinator(void)
{
    if (g_runtime.initialized == 0u) {
        return NULL;
    }
    return &g_runtime.coordinator;
}
