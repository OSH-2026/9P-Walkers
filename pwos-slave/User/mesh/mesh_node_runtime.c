#include "mesh_node_runtime.h"

#include <string.h>

static int mesh_node_runtime_send_register(
    struct mesh_node_runtime *runtime,
    uint8_t next_hop);

static int mesh_node_runtime_send_neighbor_probe(
    struct mesh_node_runtime *runtime,
    uint8_t next_hop);

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
 * 直连邻居学习：只有 NEIGHBOR_PROBE_RESPONSE（以及未来 Phase 4 的
 * ASSIGN 回转）才会调用该逻辑，不会从普通 mesh frame 的 src 自动推断。
 *
 * 把 mesh_addr 写成一条 dst->dst 的 direct-table 路由，保持
 * cluster 的 next_hop 语义仍是 mesh addr。实际 addr->UART port 的映射由
 * service 在 learn_peer_port 回调里维护。
 */
static int mesh_node_runtime_refresh_direct_peer(
    struct mesh_node_runtime *runtime,
    uint8_t mesh_addr,
    uint8_t ingress_port)
{
    int rc;

    if (runtime == NULL || mesh_addr == MESH_ADDR_UNASSIGNED) {
        return 0;
    }

    rc = cluster_set_node_online(&runtime->cluster, mesh_addr, true);
    if (rc != 0) {
        return rc;
    }

    rc = cluster_add_route(&runtime->cluster, mesh_addr, mesh_addr, 1u);
    if (rc != 0) {
        return rc;
    }

    if (runtime->config.learn_peer_port != NULL &&
        ingress_port != MESH_PROCESSER_INGRESS_PORT_NONE) {
        return runtime->config.learn_peer_port(
            runtime->config.learn_peer_port_ctx,
            mesh_addr,
            ingress_port);
    }

    return 0;
}

/*
 * 收到 NEIGHBOR_PROBE_REQUEST 后，若本机已有正式地址，立即从同一
 * ingress port 回 NEIGHBOR_PROBE_RESPONSE，不使用普通转发路径。
 */
static int mesh_node_runtime_handle_probe_request(
    struct mesh_node_runtime *runtime,
    uint8_t ingress_port)
{
    uint8_t frame[MESH_PROCESSER_FRAME_CAP];
    size_t frame_len = 0u;

    if (!mesh_build_neighbor_probe_response(
            runtime->processor.config.local_addr,
            MESH_ADDR_UNASSIGNED,
            mesh_node_runtime_take_seq(runtime),
            mesh_node_runtime_default_hop(runtime),
            frame,
            sizeof(frame),
            &frame_len)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    if (runtime->config.send_frame_to_port != NULL) {
        return runtime->config.send_frame_to_port(
            runtime->config.transport_ctx,
            ingress_port,
            frame,
            frame_len);
    }

    /* 降级：使用 send_frame，next_hop 填入 ingress port id。仅当
       transport 层能正确处理该语义时才生效。 */
    return runtime->config.send_frame(
        runtime->config.transport_ctx,
        (uint8_t)ingress_port,
        frame,
        frame_len);
}

/*
 * 节点在 bootstrap 阶段 local_addr 仍是 0xFF。
 * 如果不按 UID 再过滤一次，任何发到 0xFF 的 ASSIGN 都会被误接收。
 *
 * 本机命中 ASSIGN 后：
 * - 记录 upstream_port（上游 control-plane port）
 * - 更新本机地址
 * - 向上游重发确认 REGISTER
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
        /* 记录 ASSIGN 入口端口为 upstream/control-plane port。 */


        rc = mesh_node_runtime_send_register(runtime, frame->src);
        if (rc != 0) {
            return rc;
        }

        /* 对所有端口广播 NEIGHBOR_PROBE_REQUEST，主动发现直连邻居。 */
        rc = mesh_node_runtime_send_neighbor_probe(runtime, runtime->config.bootstrap_next_hop);
        if (rc != 0) {
            return rc;
        }
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

/*
 * 向所有端口发送 NEIGHBOR_PROBE_REQUEST，用于本机 ASSIGN 完成后
 * 主动发现直连邻居。next_hop 使用 NEIGHBOR_ANY 广播语义。
 */
static int mesh_node_runtime_send_neighbor_probe(
    struct mesh_node_runtime *runtime,
    uint8_t next_hop)
{
    uint8_t frame[MESH_PROCESSER_FRAME_CAP];
    size_t frame_len = 0u;

    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED) {
        return 0;
    }

    if (!mesh_build_neighbor_probe_request(
            runtime->processor.config.local_addr,
            mesh_node_runtime_take_seq(runtime),
            mesh_node_runtime_default_hop(runtime),
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
    const struct mesh_node_runtime_config *config)
{
    struct mesh_node_runtime_config merged_config;
    struct cluster_config cluster_config;
    struct mesh_processer_config processor_config;
    int rc;

    if (runtime == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    mesh_node_runtime_get_default_config(&merged_config);
    if (config != NULL) {
        merged_config = *config;
    }
    if (merged_config.send_frame == NULL || merged_config.receive_frame == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->config = merged_config;

    cluster_get_default_config(&cluster_config);
    cluster_config.local_addr = runtime->config.local_addr;
    cluster_config.mode = CLUSTER_MODE_DIRECT_TABLE;
    rc = cluster_init(&runtime->cluster, &cluster_config);
    if (rc != 0) {
        return rc;
    }

    mesh_processer_get_default_config(&processor_config);
    processor_config.send_frame = runtime->config.send_frame;
    processor_config.receive_frame = runtime->config.receive_frame;
    processor_config.transport_ctx = runtime->config.transport_ctx;
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
        cluster_deinit(&runtime->cluster);
        return rc;
    }

    runtime->next_mesh_seq = 1u;
    runtime->initialized = true;
    runtime->last_ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;

    if (runtime->config.auto_register_on_init) {
        rc = mesh_node_runtime_send_register(runtime, runtime->config.bootstrap_next_hop);
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
    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_node_runtime_send_register(runtime, runtime->config.bootstrap_next_hop);
}

int mesh_node_runtime_process_frame_from_port(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len,
    uint8_t ingress_port)
{
    struct mesh_frame_view frame;

    if (runtime == NULL || !runtime->initialized || frame_data == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (!mesh_decode_frame(frame_data, frame_len, &frame)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    runtime->last_ingress_port = ingress_port;

    /* NEIGHBOR_PROBE_REQUEST: 只做本地应答，绝不转发。 */
    if (frame.type == MESH_TYPE_NEIGHBOR_PROBE_REQUEST) {
        if (runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED ||
            ingress_port == MESH_PROCESSER_INGRESS_PORT_NONE) {
            return 0;
        }
        return mesh_node_runtime_handle_probe_request(runtime, ingress_port);
    }

    /* NEIGHBOR_PROBE_RESPONSE: 唯一能建立直连邻居的帧类型。 */
    if (frame.type == MESH_TYPE_NEIGHBOR_PROBE_RESPONSE) {
        if (frame.src == MESH_ADDR_UNASSIGNED ||
            ingress_port == MESH_PROCESSER_INGRESS_PORT_NONE) {
            return 0;
        }
        return mesh_node_runtime_refresh_direct_peer(runtime, frame.src, ingress_port);
    }

    /*
     * 其他 frame type：不再从 src 自动推断直连邻居。
     * ASSIGN 回转/学习中继下游地址由 Phase 4 的 pending registrar 完成；
     * 当前只做正常处理或转发。
     */
    return mesh_processer_process_frame_from_port(
        &runtime->processor,
        frame_data,
        frame_len,
        ingress_port);
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

int mesh_node_runtime_poll_once(struct mesh_node_runtime *runtime)
{
    size_t rx_len = 0u;
    uint8_t ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;
    int rc;

    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = runtime->config.receive_frame(
        runtime->config.transport_ctx,
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
