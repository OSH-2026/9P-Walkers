#include "mesh_node_runtime.h"

#include "../app/mesh_diag.h"

#include <stdio.h>
#include <string.h>

#define MESH_NODE_RUNTIME_NEIGHBOR_PROBE_RETRIES 5u
#define MESH_NODE_RUNTIME_NEIGHBOR_PROBE_RETRY_MS 200u

#define MESH_NODE_RUNTIME_CONTROLLER_ADDR 0x00u

static int mesh_node_runtime_send_register_on_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id);
static int mesh_node_runtime_send_link_state(
    struct mesh_node_runtime *runtime,
    uint8_t neighbor,
    uint8_t report_port,
    bool link_up);

static int mesh_node_runtime_send_wifi_frame(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len)
{
    (void)next_hop;
    return mesh_wifi_send_frame((struct mesh_wifi *)transport_ctx, tx_data, tx_len);
}

static int mesh_node_runtime_receive_wifi_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    return mesh_wifi_receive_frame((struct mesh_wifi *)transport_ctx, rx_data, rx_cap, rx_len);
}

static int mesh_node_runtime_send_neighbor_probe(
    struct mesh_node_runtime *runtime,
    uint8_t next_hop);

static int mesh_node_runtime_send_neighbor_probe_to_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id);

static int mesh_node_runtime_send_link_state_to_upstream(
    struct mesh_node_runtime *runtime,
    uint8_t neighbor_addr);

static int mesh_node_runtime_probe_all_ports(struct mesh_node_runtime *runtime);

static uint16_t mesh_node_runtime_take_seq(struct mesh_node_runtime *runtime)
{
    uint16_t seq;

    seq = runtime->next_mesh_seq;
    ++runtime->next_mesh_seq;
    if (runtime->next_mesh_seq == 0u) {
        runtime->next_mesh_seq = 1u;
    }
    return seq;
}

static uint8_t mesh_node_runtime_default_hop(const struct mesh_node_runtime *runtime)
{
    if (runtime == NULL || runtime->config.default_hop == 0u) {
        return MESH_PROCESSER_DEFAULT_HOP;
    }

    return runtime->config.default_hop;
}

/*
 * 点对点 UART 场景下，只要某个对端曾经在这条链路上给我发过帧，
 * 就说明它当前可以通过“本链路直达”。
 *
 * 因此这里把 src 直接写成一条 dst->dst 的 direct-table 路由。
 * send_frame 具体是否真的使用 next_hop，由底层 transport 决定；
 * 但 cluster 至少需要知道“这个地址可以从当前链路发出去”。
 */
static int mesh_node_runtime_refresh_direct_peer(
    struct mesh_node_runtime *runtime,
    uint8_t mesh_addr)
{
    uint8_t previous_selector = MESH_NODE_RUNTIME_INVALID_PORT;
    bool previous_local = false;
    bool route_changed = true;
    int rc;

    if (out_route_changed != NULL) {
        *out_route_changed = false;
    }
    if (runtime == NULL || mesh_addr == MESH_ADDR_UNASSIGNED ||
        ingress_port == MESH_NODE_RUNTIME_INVALID_PORT) {
        return 0;
    }

    rc = cluster_lookup_next_hop(&runtime->cluster, mesh_addr, &previous_selector, &previous_local);
    if (rc == 0 && !previous_local && previous_selector == ingress_port) {
        route_changed = false;
    }

    rc = cluster_set_node_online(&runtime->cluster, mesh_addr, true);
    if (rc != 0) {
        return rc;
    }

    return cluster_add_route(&runtime->cluster, mesh_addr, mesh_addr, 1u);
}

/*
 * 节点在 bootstrap 阶段 local_addr 仍是 0xFF。
 * 如果不按 UID 再过滤一次，任何发到 0xFF 的 ASSIGN 都会被误接收。
 *
 * 本机命中 ASSIGN 后：
 * - 记录 upstream_port（上游 control-plane port）
 * - 更新本机地址
 * - 对所有端口发起 NEIGHBOR_PROBE_REQUEST
 */
static int mesh_node_runtime_control_handler(
    void *runtime_ctx,
    const struct mesh_frame_view *frame,
    uint8_t *out_reply_frame,
    size_t reply_cap,
    size_t *out_reply_len)
{
    struct mesh_node_runtime *runtime = (struct mesh_node_runtime *)runtime_ctx;
    struct mesh_assign_payload assign_payload;
    bool assign_hits_local = false;
    int rc;

    if (runtime == NULL || frame == NULL || out_reply_len == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    if (frame->type == MESH_TYPE_ASSIGN) {
        if (!mesh_parse_assign(frame, &assign_payload)) {
            return -(int)MESH_ERR_BAD_FRAME;
        }
        if (memcmp(assign_payload.uid, runtime->config.local_uid, MESH_UID_LEN) != 0) {
            mesh_diag_text("assign foreign uid");
            *out_reply_len = 0u;
            return 0;
        }
        mesh_diag_text("assign local hit");
        assign_hits_local = true;
    }

    rc = cluster_processor_control_handler(
        &runtime->cluster,
        frame,
        out_reply_frame,
        reply_cap,
        out_reply_len);
    if (rc != 0) {
        return rc;
    }

    if (assign_hits_local) {
        runtime->processor.config.local_addr = assign_payload.node_addr;
        rc = mesh_node_runtime_send_register(runtime, frame->src);
        if (rc != 0) {
            return rc;
        }
        runtime->neighbor_probe_retries_left = MESH_NODE_RUNTIME_NEIGHBOR_PROBE_RETRIES;
        runtime->next_neighbor_probe_ms =
            mesh_node_runtime_time_ms(runtime) + MESH_NODE_RUNTIME_NEIGHBOR_PROBE_RETRY_MS;
    }

    return 0;
}

static int mesh_node_runtime_send_register(
    struct mesh_node_runtime *runtime,
    uint8_t next_hop)
{
    struct mesh_register_payload payload;
    uint8_t frame[MESH_PROCESSER_FRAME_CAP];
    size_t frame_len = 0u;

    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    memset(&payload, 0, sizeof(payload));
    memcpy(payload.uid, runtime->config.local_uid, sizeof(payload.uid));
    payload.boot_nonce = runtime->config.boot_nonce;
    payload.capability_bits = runtime->config.capability_bits;
    payload.port_bitmap = runtime->config.port_bitmap;

    if (!mesh_build_register(
            runtime->processor.config.local_addr,
            mesh_node_runtime_take_seq(runtime),
            mesh_node_runtime_default_hop(runtime),
            &payload,
            frame,
            sizeof(frame),
            &frame_len)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    return runtime->config.send_frame(
        runtime->config.transport_ctx,
        next_hop,
        frame,
        frame_len);
}

void mesh_node_runtime_get_default_config(struct mesh_node_runtime_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->local_addr = MESH_ADDR_UNASSIGNED;
    out_config->default_hop = MESH_PROCESSER_DEFAULT_HOP;
    out_config->auto_register_on_init = true;
}

int mesh_node_runtime_init(
    struct mesh_node_runtime *runtime,
    const struct mesh_node_runtime_config *config,
    size_t port_count,
    bool wifi_supported)
{
    struct mesh_node_runtime_config merged_config;
    struct cluster_config cluster_config;
    struct mesh_processer_config processor_config;
    struct mesh_node_runtime_port_config wifi_port;
    size_t i;
    int rc;

    if (runtime == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if ((port_count == 0u && !wifi_supported) || port_count > MESH_NODE_RUNTIME_MAX_UART_PORTS) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    mesh_node_runtime_get_default_config(&merged_config);
    if (config != NULL) {
        merged_config = *config;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->config = merged_config;
    runtime->port_count = 0u;
    runtime->active_rx_port = MESH_NODE_RUNTIME_INVALID_PORT;
    runtime->control_plane_port = MESH_NODE_RUNTIME_INVALID_PORT;
    runtime->wifi_supported = wifi_supported;

    if (port_count > 0u && runtime->config.ports != NULL) {
        for (i = 0u; i < port_count; ++i) {
            rc = mesh_node_runtime_register_port(runtime, i, &runtime->config.ports[i]);
            if (rc != 0) {
                mesh_node_runtime_deinit(runtime);
                return rc;
            }
        }
    } else if (port_count > 0u) {
        struct mesh_node_runtime_port_config single_port;

        if (port_count != 1u ||
            runtime->config.send_frame == NULL ||
            runtime->config.receive_frame == NULL) {
            mesh_node_runtime_deinit(runtime);
            return -(int)MESH_ERR_INVALID_STATE;
        }

        memset(&single_port, 0, sizeof(single_port));
        single_port.send_frame = runtime->config.send_frame;
        single_port.receive_frame = runtime->config.receive_frame;
        single_port.transport_ctx = runtime->config.transport_ctx;
        single_port.port_id = 0u;
        rc = mesh_node_runtime_register_port(runtime, 0u, &single_port);
        if (rc != 0) {
            mesh_node_runtime_deinit(runtime);
            return rc;
        }
    }

    if (wifi_supported) {
        if (runtime->config.wifi_config == NULL) {
            mesh_node_runtime_deinit(runtime);
            return -(int)MESH_ERR_INVALID_STATE;
        }

        rc = mesh_wifi_init(&runtime->wifi, runtime->config.wifi_config);
        if (rc != 0) {
            mesh_node_runtime_deinit(runtime);
            return rc;
        }

        memset(&wifi_port, 0, sizeof(wifi_port));
        wifi_port.send_frame = mesh_node_runtime_send_wifi_frame;
        wifi_port.receive_frame = mesh_node_runtime_receive_wifi_frame;
        wifi_port.transport_ctx = &runtime->wifi;
        wifi_port.port_id = MESH_NODE_RUNTIME_WIFI_PORT_ID;
        rc = mesh_node_runtime_register_port(runtime, runtime->port_count, &wifi_port);
        if (rc != 0) {
            mesh_node_runtime_deinit(runtime);
            return rc;
        }
    }

    if (runtime->config.port_bitmap == 0u) {
        runtime->config.port_bitmap = mesh_node_runtime_build_port_bitmap(runtime, &rc);
        if (rc != 0) {
            mesh_node_runtime_deinit(runtime);
            return rc;
        }
    } else if (wifi_supported) {
        runtime->config.port_bitmap = (uint8_t)(runtime->config.port_bitmap | CLUSTER_PORT_WIFI_MASK);
    } else {
        runtime->config.port_bitmap = (uint8_t)(runtime->config.port_bitmap & (uint8_t)~CLUSTER_PORT_WIFI_MASK);
    }

    cluster_get_default_config(&cluster_config);
    cluster_config.local_addr = runtime->config.local_addr;
    cluster_config.mode = CLUSTER_MODE_DIRECT_TABLE;
    cluster_config.direct_routes_use_port_selectors = true;
    rc = cluster_init(&runtime->cluster, &cluster_config);
    if (rc != 0) {
        mesh_node_runtime_deinit(runtime);
        return rc;
    }

    mesh_processer_get_default_config(&processor_config);
    processor_config.send_frame = mesh_node_runtime_send_selected_port;
    processor_config.receive_frame = mesh_node_runtime_receive_from_any_port;
    processor_config.transport_ctx = runtime;
    processor_config.route_lookup = cluster_processor_route_lookup;
    processor_config.cluster_ctx = &runtime->cluster;
    processor_config.control_handler = mesh_node_runtime_control_handler;
    processor_config.control_handler_ctx = runtime;
    processor_config.mini9p_server_handler = runtime->config.mini9p_server_handler;
    processor_config.mini9p_server_ctx = runtime->config.mini9p_server_ctx;
    processor_config.local_addr = runtime->config.local_addr;
    processor_config.default_hop = mesh_node_runtime_default_hop(runtime);
    rc = mesh_processer_init(&runtime->processor, &processor_config);
    if (rc != 0) {
        mesh_node_runtime_deinit(runtime);
        return rc;
    }

    runtime->next_mesh_seq = 1u;
    runtime->initialized = true;
    runtime->last_ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;
    runtime->upstream_port = MESH_PROCESSER_INGRESS_PORT_NONE;
    runtime->control_plane_addr = MESH_ADDR_UNASSIGNED;
    runtime->upstream_peer_addr = MESH_ADDR_UNASSIGNED;

    if (runtime->config.auto_register_on_init) {
        rc = mesh_node_runtime_notify_link_up(runtime);
        if (rc != 0) {
            mesh_node_runtime_deinit(runtime);
            return rc;
        }
    }

    return 0;
}

void mesh_node_runtime_deinit(struct mesh_node_runtime *runtime)
{
    if (runtime == NULL) {
        return;
    }

    mesh_processer_deinit(&runtime->processor);
    mesh_wifi_deinit(&runtime->wifi);
    cluster_deinit(&runtime->cluster);
    memset(runtime, 0, sizeof(*runtime));
}

int mesh_node_runtime_notify_link_up(struct mesh_node_runtime *runtime)
{
    size_t i;

    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    for (i = 0u; i < runtime->port_count; ++i) {
        int rc = mesh_node_runtime_send_register_on_port(runtime, runtime->ports[i].port_id);

        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

int mesh_node_runtime_notify_link_up_on_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id)
{
    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_runtime_send_register_on_port(runtime, port_id);
}

int mesh_node_runtime_process_frame_from_port(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len)
{
    struct mesh_frame_view frame;
    int rc;

    if (runtime == NULL || !runtime->initialized || frame_data == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (!mesh_decode_frame(frame_data, frame_len, &frame)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    rc = mesh_node_runtime_refresh_direct_peer(runtime, frame.src);
    if (rc != 0) {
        return rc;
    }

    return mesh_processer_process_frame(&runtime->processor, frame_data, frame_len);
}

int mesh_node_runtime_poll_once(struct mesh_node_runtime *runtime)
{
    size_t rx_len = 0u;
    int rc;

    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = mesh_node_runtime_receive_from_any_port(
        runtime,
        runtime->processor.rx_buffer,
        sizeof(runtime->processor.rx_buffer),
        &rx_len,
        &ingress_port);
    if (rc != 0) {
        if (rc == -(int)MESH_ERR_BUSY) {
            int retry_rc = mesh_node_runtime_poll_neighbor_probe_retry(runtime);

            if (retry_rc == 0) {
                return 0;
            }
        }
        return rc;
    }

    return mesh_node_runtime_process_frame(runtime, runtime->processor.rx_buffer, rx_len);
}