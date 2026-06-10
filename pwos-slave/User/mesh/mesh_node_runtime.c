#include "mesh_node_runtime.h"

#include "../app/mesh_diag.h"

#include <stdio.h>
#include <string.h>

#define MESH_NODE_RUNTIME_NEIGHBOR_PROBE_RETRIES 5u
#define MESH_NODE_RUNTIME_NEIGHBOR_PROBE_RETRY_MS 200u

/* 未分配地址期间 REGISTER 重发：起始间隔与退避封顶（毫秒）。 */
#define MESH_NODE_RUNTIME_REGISTER_RETRY_MS 250u
#define MESH_NODE_RUNTIME_REGISTER_RETRY_MAX_MS 1000u

static int mesh_node_runtime_send_register(
    struct mesh_node_runtime *runtime,
    uint8_t next_hop);

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

static uint32_t mesh_node_runtime_time_ms(struct mesh_node_runtime *runtime)
{
    if (runtime != NULL && runtime->config.time_ms != NULL) {
        return runtime->config.time_ms(runtime->config.time_ctx);
    }

    return 0u;
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

static bool mesh_node_runtime_has_direct_peer(
    struct mesh_node_runtime *runtime,
    uint8_t mesh_addr)
{
    uint8_t next_hop = 0u;
    bool is_local = false;

    if (runtime == NULL || mesh_addr == MESH_ADDR_UNASSIGNED) {
        return false;
    }

    return cluster_lookup_next_hop(&runtime->cluster, mesh_addr, &next_hop, &is_local) == 0 &&
        !is_local &&
        next_hop == mesh_addr;
}

static int mesh_node_runtime_ensure_upstream_control_route(struct mesh_node_runtime *runtime)
{
    uint8_t next_hop = 0u;
    bool is_local = false;
    int rc;

    if (runtime == NULL ||
        runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED ||
        runtime->control_plane_addr == MESH_ADDR_UNASSIGNED ||
        runtime->upstream_peer_addr == MESH_ADDR_UNASSIGNED ||
        runtime->control_plane_addr == runtime->processor.config.local_addr) {
        return 0;
    }

    rc = cluster_lookup_next_hop(
        &runtime->cluster,
        runtime->control_plane_addr,
        &next_hop,
        &is_local);
    if (rc == 0 || rc != -(int)MESH_ERR_NO_ROUTE) {
        return rc;
    }

    return cluster_add_route(
        &runtime->cluster,
        runtime->control_plane_addr,
        runtime->upstream_peer_addr,
        1u);
}

static int mesh_node_runtime_report_new_direct_peer(
    struct mesh_node_runtime *runtime,
    uint8_t peer_addr,
    bool already_known)
{
    if (already_known) {
        return 0;
    }
    if (runtime == NULL ||
        runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED ||
        runtime->control_plane_addr == MESH_ADDR_UNASSIGNED ||
        runtime->upstream_port == MESH_PROCESSER_INGRESS_PORT_NONE ||
        runtime->config.send_frame_to_port == NULL ||
        peer_addr == MESH_ADDR_UNASSIGNED) {
        mesh_diag_text("direct peer skip link_state");
        return 0;
    }

    return mesh_node_runtime_send_link_state_to_upstream(runtime, peer_addr);
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

static struct mesh_node_runtime_bootstrap_pending *mesh_node_runtime_find_pending_by_uid(
    struct mesh_node_runtime *runtime,
    const uint8_t uid[MESH_UID_LEN])
{
    size_t i;

    if (runtime == NULL || uid == NULL) {
        return NULL;
    }

    for (i = 0u; i < MESH_NODE_RUNTIME_MAX_BOOTSTRAP_PENDING; ++i) {
        struct mesh_node_runtime_bootstrap_pending *entry = &runtime->pending_bootstrap[i];

        if (entry->used && memcmp(entry->uid, uid, MESH_UID_LEN) == 0) {
            return entry;
        }
    }

    return NULL;
}

static struct mesh_node_runtime_bootstrap_pending *mesh_node_runtime_alloc_pending(
    struct mesh_node_runtime *runtime)
{
    size_t i;

    if (runtime == NULL) {
        return NULL;
    }

    for (i = 0u; i < MESH_NODE_RUNTIME_MAX_BOOTSTRAP_PENDING; ++i) {
        if (!runtime->pending_bootstrap[i].used) {
            return &runtime->pending_bootstrap[i];
        }
    }

    return NULL;
}

static int mesh_node_runtime_remember_pending_register(
    struct mesh_node_runtime *runtime,
    const struct mesh_register_payload *payload,
    uint8_t ingress_port)
{
    struct mesh_node_runtime_bootstrap_pending *entry;

    if (runtime == NULL || payload == NULL ||
        ingress_port == MESH_PROCESSER_INGRESS_PORT_NONE) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    entry = mesh_node_runtime_find_pending_by_uid(runtime, payload->uid);
    if (entry == NULL) {
        entry = mesh_node_runtime_alloc_pending(runtime);
    }
    if (entry == NULL) {
        return -(int)MESH_ERR_BUSY;
    }

    entry->used = true;
    memcpy(entry->uid, payload->uid, sizeof(entry->uid));
    entry->boot_nonce = payload->boot_nonce;
    entry->ingress_port = ingress_port;
    return 0;
}

static int mesh_node_runtime_send_raw_to_upstream(
    struct mesh_node_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len)
{
    if (runtime == NULL || frame_data == NULL || frame_len == 0u ||
        runtime->upstream_port == MESH_PROCESSER_INGRESS_PORT_NONE ||
        runtime->config.send_frame_to_port == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return runtime->config.send_frame_to_port(
        runtime->config.transport_ctx,
        runtime->upstream_port,
        frame_data,
        frame_len);
}

static int mesh_node_runtime_handle_downstream_register(
    struct mesh_node_runtime *runtime,
    const struct mesh_frame_view *frame,
    const uint8_t *frame_data,
    size_t frame_len,
    uint8_t ingress_port)
{
    struct mesh_register_payload payload;
    int rc;

    if (runtime == NULL || frame == NULL || frame_data == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED ||
        ingress_port == MESH_PROCESSER_INGRESS_PORT_NONE ||
        runtime->upstream_port == MESH_PROCESSER_INGRESS_PORT_NONE) {
        return 0;
    }
    if (!mesh_parse_register(frame, &payload)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    rc = mesh_node_runtime_remember_pending_register(runtime, &payload, ingress_port);
    if (rc != 0) {
        return rc;
    }

    return mesh_node_runtime_send_raw_to_upstream(runtime, frame_data, frame_len);
}

static int mesh_node_runtime_handle_pending_assign(
    struct mesh_node_runtime *runtime,
    const struct mesh_frame_view *frame,
    const uint8_t *frame_data,
    size_t frame_len)
{
    struct mesh_node_runtime_bootstrap_pending *pending;
    struct mesh_assign_payload payload;
    int rc;

    if (runtime == NULL || frame == NULL || frame_data == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (!mesh_parse_assign(frame, &payload)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    pending = mesh_node_runtime_find_pending_by_uid(runtime, payload.uid);
    if (pending == NULL) {
        return -(int)MESH_ERR_NO_ROUTE;
    }
    if (runtime->config.send_frame_to_port == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = runtime->config.send_frame_to_port(
        runtime->config.transport_ctx,
        pending->ingress_port,
        frame_data,
        frame_len);
    if (rc != 0) {
        return rc;
    }

    rc = mesh_node_runtime_refresh_direct_peer(runtime, payload.node_addr, pending->ingress_port);
    if (rc != 0) {
        return rc;
    }

    rc = mesh_node_runtime_send_link_state_to_upstream(runtime, payload.node_addr);
    if (rc != 0) {
        return rc;
    }

    memset(pending, 0, sizeof(*pending));
    return 0;
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
        runtime->upstream_port = runtime->last_ingress_port;
        runtime->control_plane_addr = frame->src;

        rc = mesh_node_runtime_probe_all_ports(runtime);
        mesh_diag_kv_u32("assign probe rc", (uint32_t)(int32_t)rc);
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

static int mesh_node_runtime_send_link_state_to_upstream(
    struct mesh_node_runtime *runtime,
    uint8_t neighbor_addr)
{
    struct mesh_link_state_payload payload;
    uint8_t frame[MESH_PROCESSER_FRAME_CAP];
    size_t frame_len = 0u;

    if (runtime == NULL || !runtime->initialized ||
        runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED ||
        runtime->control_plane_addr == MESH_ADDR_UNASSIGNED ||
        runtime->upstream_port == MESH_PROCESSER_INGRESS_PORT_NONE ||
        runtime->config.send_frame_to_port == NULL ||
        neighbor_addr == MESH_ADDR_UNASSIGNED) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    payload.neighbor = neighbor_addr;
    payload.link_up = 1u;
    payload.quality = 1u;

    if (!mesh_build_link_state(
            runtime->processor.config.local_addr,
            runtime->control_plane_addr,
            mesh_node_runtime_take_seq(runtime),
            mesh_node_runtime_default_hop(runtime),
            &payload,
            frame,
            sizeof(frame),
            &frame_len)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    return runtime->config.send_frame_to_port(
        runtime->config.transport_ctx,
        runtime->upstream_port,
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

static int mesh_node_runtime_send_neighbor_probe_to_port(
    struct mesh_node_runtime *runtime,
    uint8_t port_id)
{
    uint8_t frame[MESH_PROCESSER_FRAME_CAP];
    size_t frame_len = 0u;

    if (runtime == NULL || !runtime->initialized ||
        runtime->config.send_frame_to_port == NULL ||
        runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED) {
        return -(int)MESH_ERR_INVALID_STATE;
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

    return runtime->config.send_frame_to_port(
        runtime->config.transport_ctx,
        port_id,
        frame,
        frame_len);
}

static int mesh_node_runtime_probe_all_ports(struct mesh_node_runtime *runtime)
{
    int rc = 0;

    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    /*
     * 先探测非上游端口，最后再探测上游端口。
     * 否则 host 很快回 PROBE_RESPONSE 时，本机还在同步向其他 UART 发包，
     * 可能错过上游链路上的返回帧。
     */
    if (runtime->config.send_frame_to_port != NULL) {
        uint8_t port_id;

        for (port_id = 0u; port_id < 8u; ++port_id) {
            if ((runtime->config.port_bitmap & (uint8_t)(1u << port_id)) == 0u ||
                port_id == runtime->upstream_port) {
                continue;
            }
            rc = mesh_node_runtime_send_neighbor_probe_to_port(runtime, port_id);
            if (rc != 0) {
                return rc;
            }
        }
        if (runtime->upstream_port != MESH_PROCESSER_INGRESS_PORT_NONE &&
            (runtime->config.port_bitmap & (uint8_t)(1u << runtime->upstream_port)) != 0u) {
            rc = mesh_node_runtime_send_neighbor_probe_to_port(runtime, runtime->upstream_port);
        }
        return rc;
    }

    return mesh_node_runtime_send_neighbor_probe(runtime, runtime->config.bootstrap_next_hop);
}

static int mesh_node_runtime_poll_neighbor_probe_retry(struct mesh_node_runtime *runtime)
{
    uint32_t now;
    int rc;

    if (runtime == NULL || !runtime->initialized ||
        runtime->neighbor_probe_retries_left == 0u ||
        runtime->config.time_ms == NULL) {
        return -(int)MESH_ERR_BUSY;
    }
    if (runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED) {
        return -(int)MESH_ERR_BUSY;
    }

    now = mesh_node_runtime_time_ms(runtime);
    if ((int32_t)(now - runtime->next_neighbor_probe_ms) < 0) {
        return -(int)MESH_ERR_BUSY;
    }

    rc = mesh_node_runtime_probe_all_ports(runtime);
    if (rc == 0) {
        --runtime->neighbor_probe_retries_left;
        runtime->next_neighbor_probe_ms = now + MESH_NODE_RUNTIME_NEIGHBOR_PROBE_RETRY_MS;
    }
    return rc;
}

/*
 * 未分配地址期间按退避重发 bootstrap REGISTER。
 *
 * 这是 P0 可靠性的核心：在拿到地址（收到 ASSIGN）之前，链路上任意一帧 REGISTER
 * 或 ASSIGN 丢失都不应让节点永久卡死。本函数与 neighbor probe 重试共用
 * time_ms + 空闲轮询的模式：
 * - 只在 local_addr 仍为 UNASSIGNED 时工作；一旦分配成功即自动停止；
 * - 间隔从 REGISTER_RETRY_MS 起，每次翻倍至 REGISTER_RETRY_MAX_MS 封顶；
 * - 不设次数上限：没有地址的节点没有意义，必须一直尝试。
 *
 * 主机侧 ASSIGN 控制器对同一 UID 幂等（见 6.10 日志），因此反复重发 REGISTER
 * 不会造成主机地址漂移或重复挂载。
 */
static int mesh_node_runtime_poll_register_retry(struct mesh_node_runtime *runtime)
{
    uint32_t now;
    int rc;

    if (runtime == NULL || !runtime->initialized ||
        runtime->config.time_ms == NULL) {
        return -(int)MESH_ERR_BUSY;
    }
    if (runtime->processor.config.local_addr != MESH_ADDR_UNASSIGNED) {
        return -(int)MESH_ERR_BUSY;
    }

    now = mesh_node_runtime_time_ms(runtime);
    if ((int32_t)(now - runtime->next_register_ms) < 0) {
        return -(int)MESH_ERR_BUSY;
    }

    rc = mesh_node_runtime_send_register(runtime, runtime->config.bootstrap_next_hop);
    if (rc == 0) {
        uint32_t interval = runtime->register_retry_interval_ms;

        if (interval == 0u) {
            interval = MESH_NODE_RUNTIME_REGISTER_RETRY_MS;
        }
        interval <<= 1;
        if (interval > MESH_NODE_RUNTIME_REGISTER_RETRY_MAX_MS) {
            interval = MESH_NODE_RUNTIME_REGISTER_RETRY_MAX_MS;
        }
        runtime->register_retry_interval_ms = interval;
        runtime->next_register_ms = now + interval;
    }
    return rc;
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
    runtime->upstream_port = MESH_PROCESSER_INGRESS_PORT_NONE;
    runtime->control_plane_addr = MESH_ADDR_UNASSIGNED;
    runtime->upstream_peer_addr = MESH_ADDR_UNASSIGNED;

    /*
     * 安排未分配期间的首次 REGISTER 重发。init 里（auto_register_on_init）已经
     * 立即发了一帧；这里把首次"重发"排在一个基础间隔之后。一旦收到 ASSIGN，
     * local_addr 不再是 UNASSIGNED，重发分支自动停止。
     */
    runtime->register_retry_interval_ms = MESH_NODE_RUNTIME_REGISTER_RETRY_MS;
    runtime->next_register_ms =
        mesh_node_runtime_time_ms(runtime) + MESH_NODE_RUNTIME_REGISTER_RETRY_MS;

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
    if (frame.type == MESH_TYPE_ASSIGN || frame.type == MESH_TYPE_NEIGHBOR_PROBE_RESPONSE) {
        char diag[96];

        (void)snprintf(
            diag,
            sizeof(diag),
            "frame type=0x%02x src=0x%02x dst=0x%02x port=%u",
            frame.type,
            frame.src,
            frame.dst,
            (unsigned)ingress_port);
        mesh_diag_text(diag);
    }

    /* NEIGHBOR_PROBE_REQUEST: 只做本地应答，绝不转发。 */
    if (frame.type == MESH_TYPE_NEIGHBOR_PROBE_REQUEST) {
        int rc;
        bool already_known;

        if (runtime->processor.config.local_addr == MESH_ADDR_UNASSIGNED ||
            ingress_port == MESH_PROCESSER_INGRESS_PORT_NONE) {
            return 0;
        }
        already_known = mesh_node_runtime_has_direct_peer(runtime, frame.src);
        if (frame.src != MESH_ADDR_UNASSIGNED) {
            rc = mesh_node_runtime_refresh_direct_peer(runtime, frame.src, ingress_port);
            if (rc != 0) {
                return rc;
            }
            if (ingress_port == runtime->upstream_port) {
                runtime->upstream_peer_addr = frame.src;
                runtime->neighbor_probe_retries_left = 0u;
                rc = mesh_node_runtime_ensure_upstream_control_route(runtime);
                if (rc != 0) {
                    return rc;
                }
            }
            rc = mesh_node_runtime_report_new_direct_peer(runtime, frame.src, already_known);
            if (rc != 0) {
                return rc;
            }
        }
        return mesh_node_runtime_handle_probe_request(runtime, ingress_port);
    }

    /* NEIGHBOR_PROBE_RESPONSE: 唯一能建立直连邻居的帧类型。 */
    if (frame.type == MESH_TYPE_NEIGHBOR_PROBE_RESPONSE) {
        int rc;
        char diag[96];
        bool already_known;

        if (frame.src == MESH_ADDR_UNASSIGNED ||
            ingress_port == MESH_PROCESSER_INGRESS_PORT_NONE) {
            return 0;
        }
        already_known = mesh_node_runtime_has_direct_peer(runtime, frame.src);
        (void)snprintf(
            diag,
            sizeof(diag),
            "probe resp src=0x%02x port=%u",
            frame.src,
            (unsigned)ingress_port);
        mesh_diag_text(diag);
        rc = mesh_node_runtime_refresh_direct_peer(runtime, frame.src, ingress_port);
        if (rc != 0) {
            mesh_diag_kv_u32("probe resp learn rc", (uint32_t)(int32_t)rc);
            return rc;
        }
        if (ingress_port == runtime->upstream_port) {
            runtime->upstream_peer_addr = frame.src;
            runtime->neighbor_probe_retries_left = 0u;
            rc = mesh_node_runtime_ensure_upstream_control_route(runtime);
            if (rc != 0) {
                return rc;
            }
        }
        rc = mesh_node_runtime_report_new_direct_peer(runtime, frame.src, already_known);
        (void)snprintf(
            diag,
            sizeof(diag),
            "link_state rc=%d up=%u ctrl=0x%02x",
            rc,
            (unsigned)runtime->upstream_port,
            runtime->control_plane_addr);
        mesh_diag_text(diag);
        return rc;
    }

    if (frame.type == MESH_TYPE_REGISTER &&
        frame.src == MESH_ADDR_UNASSIGNED &&
        frame.dst == MESH_ADDR_UNASSIGNED) {
        return mesh_node_runtime_handle_downstream_register(
            runtime,
            &frame,
            frame_data,
            frame_len,
            ingress_port);
    }

    if (frame.type == MESH_TYPE_LINK_STATE &&
        frame.src != runtime->processor.config.local_addr &&
        frame.dst == runtime->control_plane_addr &&
        runtime->upstream_port != MESH_PROCESSER_INGRESS_PORT_NONE) {
        return mesh_node_runtime_send_raw_to_upstream(runtime, frame_data, frame_len);
    }

    if (frame.type == MESH_TYPE_ASSIGN) {
        struct mesh_assign_payload payload;

        if (!mesh_parse_assign(&frame, &payload)) {
            return -(int)MESH_ERR_BAD_FRAME;
        }
        if (mesh_node_runtime_find_pending_by_uid(runtime, payload.uid) != NULL) {
            return mesh_node_runtime_handle_pending_assign(runtime, &frame, frame_data, frame_len);
        }
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
        if (rc == -(int)MESH_ERR_BUSY) {
            /*
             * 空闲（无入站帧）时推进控制面重传：未分配则重发 REGISTER，已分配则
             * 重试 neighbor probe。二者按 local_addr 互斥，至多一个真正动作。
             */
            int retry_rc = mesh_node_runtime_poll_register_retry(runtime);

            if (retry_rc != 0) {
                retry_rc = mesh_node_runtime_poll_neighbor_probe_retry(runtime);
            }
            if (retry_rc == 0) {
                return 0;
            }
        }
        return rc;
    }

    return mesh_node_runtime_process_frame_from_port(
        runtime,
        runtime->processor.rx_buffer,
        rx_len,
        ingress_port);
}

int mesh_node_runtime_format_routes(
    struct mesh_node_runtime *runtime,
    char *out,
    size_t out_cap)
{
    size_t used = 0u;
    size_t i;
    int written;

    if (runtime == NULL || out == NULL || out_cap == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    written = snprintf(
        out,
        out_cap,
        "local=0x%02x control=0x%02x upstream_port=%u upstream_peer=0x%02x\n",
        runtime->processor.config.local_addr,
        runtime->control_plane_addr,
        (unsigned)runtime->upstream_port,
        runtime->upstream_peer_addr);
    if (written < 0) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    used = (size_t)written < out_cap ? (size_t)written : out_cap - 1u;

    for (i = 0u; i < CLUSTER_MAX_ROUTES && used < out_cap - 1u; ++i) {
        const struct cluster_route *route = &runtime->cluster.routes[i];

        if (!route->valid) {
            continue;
        }

        written = snprintf(
            out + used,
            out_cap - used,
            "route dst=0x%02x next=0x%02x metric=%u local=%u\n",
            route->dst,
            route->next_hop,
            route->metric,
            route->local ? 1u : 0u);
        if (written < 0) {
            return -(int)MESH_ERR_BAD_FRAME;
        }
        if ((size_t)written >= out_cap - used) {
            used = out_cap - 1u;
            break;
        }
        used += (size_t)written;
    }

    out[used] = '\0';
    return 0;
}
