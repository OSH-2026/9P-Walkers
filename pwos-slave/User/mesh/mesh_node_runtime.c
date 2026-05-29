#include "mesh_node_runtime.h"

#include <string.h>

#define MESH_NODE_RUNTIME_CONTROLLER_ADDR 0x00u

static int mesh_node_runtime_send_register_on_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id);
static int mesh_node_runtime_send_link_state(
    struct mesh_node_runtime *runtime,
    uint8_t neighbor,
    uint8_t report_port,
    bool link_up);

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

static bool mesh_node_runtime_is_retryable_receive_error(int rc)
{
    return rc == -(int)MESH_ERR_BUSY;
}

static struct mesh_node_runtime_port *mesh_node_runtime_find_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id,
    size_t *out_index)
{
    size_t i;

    if (runtime == NULL) {
        return NULL;
    }

    for (i = 0u; i < runtime->port_count; ++i) {
        if (runtime->ports[i].initialized && runtime->ports[i].port_id == port_id) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return &runtime->ports[i];
        }
    }

    return NULL;
}

static int mesh_node_runtime_send_raw_on_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id,
    const uint8_t *frame_data,
    size_t frame_len)
{
    struct mesh_node_runtime_port *port;

    if (runtime == NULL || !runtime->initialized || frame_data == NULL || frame_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    port = mesh_node_runtime_find_port(runtime, port_id, NULL);
    if (port == NULL) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    return port->send_frame(port->transport_ctx, port->port_id, frame_data, frame_len);
}

static int mesh_node_runtime_send_selected_port(
    void *runtime_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len)
{
    return mesh_node_runtime_send_raw_on_port(
        (struct mesh_node_runtime *)runtime_ctx,
        next_hop,
        tx_data,
        tx_len);
}

static int mesh_node_runtime_receive_from_any_port(
    void *runtime_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len,
    uint8_t *out_ingress_port)
{
    struct mesh_node_runtime *runtime = (struct mesh_node_runtime *)runtime_ctx;
    size_t checked;

    if (runtime == NULL || !runtime->initialized || rx_data == NULL || rx_len == NULL ||
        out_ingress_port == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    *rx_len = 0u;
    *out_ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;

    for (checked = 0u; checked < runtime->port_count; ++checked) {
        size_t index = (runtime->next_rx_port_index + checked) % runtime->port_count;
        struct mesh_node_runtime_port *port = &runtime->ports[index];
        int rc;

        if (!port->initialized) {
            continue;
        }

        rc = port->receive_frame(port->transport_ctx, rx_data, rx_cap, rx_len, out_ingress_port);
        if (rc == 0) {
            runtime->active_rx_port = port->port_id;
            *out_ingress_port = port->port_id;
            runtime->next_rx_port_index = (index + 1u) % runtime->port_count;
            return 0;
        }
        if (!mesh_node_runtime_is_retryable_receive_error(rc)) {
            runtime->active_rx_port = MESH_NODE_RUNTIME_INVALID_PORT;
            return rc;
        }
    }

    runtime->active_rx_port = MESH_NODE_RUNTIME_INVALID_PORT;
    return -(int)MESH_ERR_BUSY;
}

static uint8_t mesh_node_runtime_build_port_bitmap(
    const struct mesh_node_runtime *runtime,
    int *out_rc)
{
    uint8_t bitmap = 0u;
    size_t i;

    if (out_rc != NULL) {
        *out_rc = 0;
    }
    if (runtime == NULL) {
        if (out_rc != NULL) {
            *out_rc = -(int)MESH_ERR_INVALID_STATE;
        }
        return 0u;
    }

    for (i = 0u; i < runtime->port_count; ++i) {
        uint8_t port_id = runtime->ports[i].port_id;

        if (port_id >= 8u) {
            if (out_rc != NULL) {
                *out_rc = -(int)MESH_ERR_BAD_FRAME;
            }
            return 0u;
        }
        bitmap = (uint8_t)(bitmap | (uint8_t)(1u << port_id));
    }

    return bitmap;
}

static int mesh_node_runtime_register_port(
    struct mesh_node_runtime *runtime,
    size_t index,
    const struct mesh_node_runtime_port_config *port_config)
{
    if (runtime == NULL || port_config == NULL || index >= MESH_NODE_RUNTIME_MAX_PORTS) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (port_config->send_frame == NULL || port_config->receive_frame == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (port_config->port_id == MESH_NODE_RUNTIME_INVALID_PORT) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (mesh_node_runtime_find_port(runtime, port_config->port_id, NULL) != NULL) {
        return -(int)MESH_ERR_BUSY;
    }

    runtime->ports[index].initialized = true;
    runtime->ports[index].port_id = port_config->port_id;
    runtime->ports[index].send_frame = port_config->send_frame;
    runtime->ports[index].receive_frame = port_config->receive_frame;
    runtime->ports[index].transport_ctx = port_config->transport_ctx;
    if (runtime->port_count <= index) {
        runtime->port_count = index + 1u;
    }
    return 0;
}

/*
 * 子机 direct-table 维护的是：某个目标节点应当从哪个本地端口发出去。
 *
 * 因此只要某个 src 曾经从某个本地端口给我发过帧，就把 src -> ingress_port
 * 记成一条静态可达路由，后续回包、转发和 ROUTE_UPDATE 下发表都共用这套选择器语义。
 */
static int mesh_node_runtime_refresh_direct_peer(
    struct mesh_node_runtime *runtime,
    uint8_t mesh_addr,
    uint8_t ingress_port,
    bool *out_route_changed)
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

    rc = cluster_add_route(&runtime->cluster, mesh_addr, ingress_port, 1u);
    if (rc != 0) {
        return rc;
    }

    if (out_route_changed != NULL) {
        *out_route_changed = route_changed;
    }
    return 0;
}

static int mesh_node_runtime_send_register_on_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id)
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

    return mesh_node_runtime_send_raw_on_port(runtime, port_id, frame, frame_len);
}

static int mesh_node_runtime_send_link_state(
    struct mesh_node_runtime *runtime,
    uint8_t neighbor,
    uint8_t report_port,
    bool link_up)
{
    struct mesh_link_state_payload payload;
    uint8_t frame[MESH_PROCESSER_FRAME_CAP];
    uint8_t tx_port;
    size_t frame_len = 0u;

    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED ||
        neighbor == MESH_ADDR_UNASSIGNED ||
        report_port == MESH_NODE_RUNTIME_INVALID_PORT) {
        return 0;
    }

    tx_port = runtime->control_plane_port;
    if (tx_port == MESH_NODE_RUNTIME_INVALID_PORT) {
        tx_port = report_port;
    }

    memset(&payload, 0, sizeof(payload));
    payload.neighbor = neighbor;
    payload.link_up = link_up ? 1u : 0u;
    payload.quality = 1u;
    payload.local_port = report_port;

    if (!mesh_build_link_state(
            runtime->processor.config.local_addr,
            MESH_NODE_RUNTIME_CONTROLLER_ADDR,
            mesh_node_runtime_take_seq(runtime),
            mesh_node_runtime_default_hop(runtime),
            &payload,
            frame,
            sizeof(frame),
            &frame_len)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    return mesh_node_runtime_send_raw_on_port(runtime, tx_port, frame, frame_len);
}

/*
 * 节点在 bootstrap 阶段 local_addr 仍是 0xFF。
 * 如果不按 UID 再过滤一次，任何发到 0xFF 的 ASSIGN 都会被误接收。
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
            *out_reply_len = 0u;
            return 0;
        }
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
        runtime->control_plane_port = runtime->active_rx_port;
        rc = mesh_node_runtime_send_register_on_port(runtime, runtime->active_rx_port);
        if (rc != 0) {
            return rc;
        }
        rc = mesh_node_runtime_send_link_state(runtime, frame->src, runtime->active_rx_port, true);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
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
    size_t port_count)
{
    struct mesh_node_runtime_config merged_config;
    struct cluster_config cluster_config;
    struct mesh_processer_config processor_config;
    size_t i;
    int rc;

    if (runtime == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (port_count == 0u || port_count > MESH_NODE_RUNTIME_MAX_UART_PORTS) {
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

    if (runtime->config.port_bitmap == 0u) {
        runtime->config.port_bitmap = mesh_node_runtime_build_port_bitmap(runtime, &rc);
        if (rc != 0) {
            mesh_node_runtime_deinit(runtime);
            return rc;
        }
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

int mesh_node_runtime_report_neighbor_link(
    struct mesh_node_runtime *runtime,
    uint8_t neighbor,
    uint8_t local_port,
    bool link_up)
{
    return mesh_node_runtime_send_link_state(runtime, neighbor, local_port, link_up);
}

int mesh_node_runtime_process_frame(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len)
{
    return mesh_node_runtime_process_frame_from_port(
        runtime,
        frame_data,
        frame_len,
        MESH_PROCESSER_INGRESS_PORT_NONE);
}

int mesh_node_runtime_process_frame_from_port(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len,
    uint8_t ingress_port)
{
    struct mesh_frame_view frame;
    bool handled = false;
    int rc;

    if (runtime == NULL || !runtime->initialized || frame_data == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (!mesh_decode_frame(frame_data, frame_len, &frame)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    if (runtime->config.bootstrap_bridge != NULL) {
        rc = runtime->config.bootstrap_bridge(
            runtime->config.bootstrap_bridge_ctx,
            runtime,
            frame_data,
            frame_len,
            &frame,
            ingress_port,
            &handled);
        if (rc != 0 || handled) {
            return rc;
        }
    }

    rc = mesh_node_runtime_refresh_direct_peer(runtime, frame.src, ingress_port, NULL);
    if (rc != 0) {
        return rc;
    }

    return mesh_processer_process_frame(&runtime->processor, frame_data, frame_len);
}

int mesh_node_runtime_poll_once(struct mesh_node_runtime *runtime)
{
    size_t rx_len = 0u;
    uint8_t ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;
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
        return rc;
    }

    return mesh_node_runtime_process_frame_from_port(
        runtime,
        runtime->processor.rx_buffer,
        rx_len,
        ingress_port);
}
