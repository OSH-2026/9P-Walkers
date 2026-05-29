#include "mesh_host_runtime.h"

#include <string.h>

/*
 * 这个文件的核心思路是：
 * - mesh_processer 继续负责“协议层分流”；
 * - mesh_host_runtime 负责“把分流结果落到主机侧状态机和同步请求模型里”。
 *
 * 也就是说：
 * 1. REGISTER / LINK_STATE 这类控制帧进来时，由 runtime 的 control_handler
 *    包装 shared cluster 控制处理器，再补上 VFS/UID/client 同步；
 * 2. 本地主机主动发 mini9P 请求时，不直接走 raw UART，而是先封成 mesh MINI9P
 *    数据面帧，再在等待响应的过程中继续替 runtime 处理途中到来的其他 mesh 帧；
 * 3. 当前实现保持“同一时刻只允许一个同步请求占用接收链路”，用最小复杂度
 *    换取一个真正可运行、可调试的 runtime 闭环。
 */

static bool m9p_type_is_response(uint8_t type)
{
    return (type & 0x80u) != 0u;
}

static bool m9p_type_is_request(uint8_t type)
{
    return (type & 0x80u) == 0u;
}

static uint16_t mesh_host_runtime_next_seq(struct mesh_host_runtime *runtime)
{
    uint16_t seq;

    seq = runtime->next_mesh_seq;
    ++runtime->next_mesh_seq;
    if (runtime->next_mesh_seq == 0u) {
        runtime->next_mesh_seq = 1u;
    }
    return seq;
}

/*
 * ESP32-P4 是双核 RISC-V。dispatch_busy 会被 poll 任务和 shell 任务从两个核并发访问，
 * 必须用原子 CAS 保护 check-then-set，否则两个核可能同时读到 false 并双双进入临界区。
 */
static int mesh_host_runtime_claim_dispatch(struct mesh_host_runtime *runtime)
{
    bool expected = false;

    if (!__atomic_compare_exchange_n(
            &runtime->dispatch_busy, &expected, true,
            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        return -(int)M9P_ERR_EBUSY;
    }

    return 0;
}

static void mesh_host_runtime_release_dispatch(struct mesh_host_runtime *runtime)
{
    __atomic_store_n(&runtime->dispatch_busy, false, __ATOMIC_RELEASE);
}

static void mesh_host_runtime_init_slot(
    struct mesh_host_runtime *runtime,
    size_t slot_index)
{
    struct mesh_host_runtime_client_slot *slot = &runtime->clients[slot_index];

    memset(slot, 0, sizeof(*slot));
    slot->mesh_addr = MESH_HOST_RUNTIME_UNASSIGNED_ADDR;
    slot->transport_ctx.runtime = runtime;
    slot->transport_ctx.slot_index = slot_index;
    m9p_client_init(&slot->client, NULL, NULL);
}

static struct mesh_host_runtime_client_slot *mesh_host_runtime_find_slot_by_uid(
    struct mesh_host_runtime *runtime,
    const uint8_t uid[MESH_UID_LEN])
{
    size_t i;

    for (i = 0u; i < MESH_HOST_RUNTIME_MAX_CLIENTS; ++i) {
        if (!runtime->clients[i].used) {
            continue;
        }
        if (memcmp(runtime->clients[i].uid, uid, MESH_UID_LEN) == 0) {
            return &runtime->clients[i];
        }
    }

    return NULL;
}

static struct mesh_host_runtime_client_slot *mesh_host_runtime_find_slot_by_addr(
    struct mesh_host_runtime *runtime,
    uint8_t mesh_addr)
{
    size_t i;

    for (i = 0u; i < MESH_HOST_RUNTIME_MAX_CLIENTS; ++i) {
        if (!runtime->clients[i].used) {
            continue;
        }
        if (runtime->clients[i].mesh_addr == mesh_addr) {
            return &runtime->clients[i];
        }
    }

    return NULL;
}

static struct mesh_host_runtime_client_slot *mesh_host_runtime_alloc_slot(
    struct mesh_host_runtime *runtime)
{
    size_t i;

    for (i = 0u; i < MESH_HOST_RUNTIME_MAX_CLIENTS; ++i) {
        struct mesh_host_runtime_client_slot *slot = &runtime->clients[i];

        if (slot->used) {
            continue;
        }

        mesh_host_runtime_init_slot(runtime, i);
        slot->used = true;
        m9p_client_init(&slot->client, NULL, &slot->transport_ctx);
        return slot;
    }

    return NULL;
}

static void mesh_host_runtime_mark_slot_addr_reused(struct mesh_host_runtime_client_slot *slot)
{
    if (slot == NULL || !slot->used) {
        return;
    }

    slot->online = false;
    slot->mesh_addr = MESH_HOST_RUNTIME_UNASSIGNED_ADDR;
    m9p_client_reset_session(&slot->client);
}

static void mesh_host_runtime_mark_slot_unreachable(struct mesh_host_runtime_client_slot *slot)
{
    if (slot == NULL || !slot->used) {
        return;
    }

    slot->online = false;
    m9p_client_reset_session(&slot->client);
}

static void mesh_host_runtime_mark_addr_online(
    struct mesh_host_runtime *runtime,
    uint8_t mesh_addr)
{
    struct mesh_host_runtime_client_slot *slot;

    slot = mesh_host_runtime_find_slot_by_addr(runtime, mesh_addr);
    if (slot == NULL) {
        return;
    }

    slot->online = true;
}

static int mesh_host_runtime_sync_slots_from_cluster(struct mesh_host_runtime *runtime)
{
    size_t i;

    for (i = 0u; i < MESH_HOST_RUNTIME_MAX_CLIENTS; ++i) {
        bool reachable = false;
        bool online = false;
        int rc;
        struct mesh_host_runtime_client_slot *slot = &runtime->clients[i];

        if (!slot->used) {
            continue;
        }
        if (slot->mesh_addr == MESH_HOST_RUNTIME_UNASSIGNED_ADDR) {
            continue;
        }

        rc = cluster_can_reach(runtime->config.mesh_cluster, slot->mesh_addr, &reachable);
        if (rc != 0) {
            return rc;
        }
        rc = cluster_get_node_online(runtime->config.mesh_cluster, slot->mesh_addr, &online);
        if (rc != 0) {
            online = false;
        }

        if (!reachable || !online) {
            mesh_host_runtime_mark_slot_unreachable(slot);
        }
    }

    return 0;
}

static int mesh_host_runtime_prepare_registered_client(
    struct mesh_host_runtime *runtime,
    uint8_t mesh_addr,
    const uint8_t uid[MESH_UID_LEN],
    struct m9p_client **out_client)
{
    struct mesh_host_runtime_client_slot *slot;
    struct mesh_host_runtime_client_slot *conflict_slot;

    conflict_slot = mesh_host_runtime_find_slot_by_addr(runtime, mesh_addr);
    if (conflict_slot != NULL && memcmp(conflict_slot->uid, uid, MESH_UID_LEN) != 0) {
        mesh_host_runtime_mark_slot_addr_reused(conflict_slot);
    }

    slot = mesh_host_runtime_find_slot_by_uid(runtime, uid);
    if (slot == NULL) {
        slot = mesh_host_runtime_alloc_slot(runtime);
        if (slot == NULL) {
            return -(int)M9P_ERR_EBUSY;
        }
    }

    memcpy(slot->uid, uid, MESH_UID_LEN);
    slot->mesh_addr = mesh_addr;
    slot->online = true;
    m9p_client_init(&slot->client, NULL, &slot->transport_ctx);

    if (out_client != NULL) {
        *out_client = &slot->client;
    }

    return 0;
}

static int mesh_host_runtime_lookup_next_hop(
    struct mesh_host_runtime *runtime,
    uint8_t dst,
    uint8_t *out_next_hop)
{
    bool is_local = false;
    int rc;

    rc = cluster_lookup_next_hop(runtime->config.mesh_cluster, dst, out_next_hop, &is_local);
    if (rc != 0) {
        return rc;
    }
    if (is_local) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return 0;
}

static int mesh_host_runtime_client_request(
    void *transport_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    struct mesh_host_runtime_client_transport_ctx *ctx = (struct mesh_host_runtime_client_transport_ctx *)transport_ctx;
    struct mesh_host_runtime *runtime;
    struct mesh_host_runtime_client_slot *slot;
    struct m9p_frame_view request_frame;
    uint8_t next_hop = 0u;
    size_t mesh_len = 0u;
    int rc;

    if (ctx == NULL || ctx->runtime == NULL || rx_data == NULL || rx_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    runtime = ctx->runtime;
    if (!runtime->initialized || ctx->slot_index >= MESH_HOST_RUNTIME_MAX_CLIENTS) {
        return -(int)M9P_ERR_EINVAL;
    }

    slot = &runtime->clients[ctx->slot_index];
    if (!slot->used || !slot->online || slot->mesh_addr == MESH_HOST_RUNTIME_UNASSIGNED_ADDR) {
        return -(int)M9P_ERR_EAGAIN;
    }
    if (!m9p_decode_frame(tx_data, tx_len, &request_frame)) {
        return -(int)M9P_ERR_EIO;
    }
    if (!m9p_type_is_request(request_frame.type)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *rx_len = 0u;

    /*
     * poll 任务在 receive_frame 内阻塞时（最长 io_timeout_ms，默认 200ms）持有
     * dispatch 锁。最多重试 25 次 × 10ms = 250ms，保证 client 请求不因轮询任务
     * 短暂占锁而立即失败。非 ESP 平台（单元测试）无 vTaskDelay，直接尝试一次。
     */
    {
        int retry;
        rc = -(int)M9P_ERR_EBUSY;
        for (retry = 0; retry < 25 && rc != 0; ++retry) {
            rc = mesh_host_runtime_claim_dispatch(runtime);
#ifdef ESP_PLATFORM
            if (rc != 0) {
                vTaskDelay(pdMS_TO_TICKS(MESH_HOST_RUNTIME_IDLE_DELAY_MS));
            }
#endif
        }
    }
    if (rc != 0) {
        return rc;
    }

    rc = mesh_host_runtime_lookup_next_hop(runtime, slot->mesh_addr, &next_hop);
    if (rc != 0) {
        goto out;
    }

    if (!mesh_build_mini9p_frame(
            runtime->config.local_addr,
            slot->mesh_addr,
            mesh_host_runtime_next_seq(runtime),
            runtime->config.default_hop,
            0u,
            tx_data,
            (uint16_t)tx_len,
            runtime->processor.tx_buffer,
            sizeof(runtime->processor.tx_buffer),
            &mesh_len)) {
        rc = -(int)M9P_ERR_EMSIZE;
        goto out;
    }

    rc = runtime->config.send_frame(
        runtime->config.transport_ctx,
        next_hop,
        runtime->processor.tx_buffer,
        mesh_len);
    if (rc != 0) {
        goto out;
    }

    while (1) {
        struct mesh_frame_view mesh_frame;
        uint8_t ingress_port = MESH_PROCESSER_INGRESS_PORT_NONE;

        rc = runtime->config.receive_frame(
            runtime->config.transport_ctx,
            runtime->processor.rx_buffer,
            sizeof(runtime->processor.rx_buffer),
            rx_len,
            &ingress_port);
        if (rc != 0) {
            goto out;
        }

        if (mesh_decode_frame(runtime->processor.rx_buffer, *rx_len, &mesh_frame) &&
            mesh_frame.dst == runtime->config.local_addr &&
            mesh_frame.type == MESH_TYPE_MINI9P &&
            mesh_frame.src == slot->mesh_addr) {
            struct m9p_frame_view response_frame;

            if (m9p_decode_frame(mesh_frame.payload, mesh_frame.payload_len, &response_frame) &&
                m9p_type_is_response(response_frame.type) &&
                response_frame.tag == request_frame.tag) {
                if (mesh_frame.payload_len > rx_cap) {
                    rc = -(int)M9P_ERR_EMSIZE;
                    goto out;
                }

                memcpy(rx_data, mesh_frame.payload, mesh_frame.payload_len);
                *rx_len = mesh_frame.payload_len;
                (void)cluster_set_node_online(runtime->config.mesh_cluster, slot->mesh_addr, true);
                slot->online = true;
                rc = 0;
                goto out;
            }
        }

        rc = mesh_host_runtime_process_frame(runtime, runtime->processor.rx_buffer, *rx_len);
        if (rc != 0) {
            goto out;
        }
    }

out:
    mesh_host_runtime_release_dispatch(runtime);
    return rc;
}

static int mesh_host_runtime_sync_registered_node(
    struct mesh_host_runtime *runtime,
    uint8_t mesh_addr,
    const uint8_t uid[MESH_UID_LEN])
{
    struct m9p_client *client = NULL;
    int rc;

    if (mesh_addr == MESH_ADDR_UNASSIGNED) {
        return 0;
    }

    rc = mesh_host_runtime_prepare_registered_client(runtime, mesh_addr, uid, &client);
    if (rc != 0) {
        return rc;
    }

    /*
     * prepare_registered_client 已通过 m9p_client_init 把 transport_ctx 指向
     * slot->transport_ctx，这里只需补上 transport 函数指针即可，不必再次查槽位。
     * 重复调用 find_slot_by_uid 并解引用其结果既冗余，又在槽位已满时存在 NULL 解引用风险。
     */
    client->transport = mesh_host_runtime_client_request;

    return cluster_config_on_mesh_node_registered(mesh_addr, uid, client, NULL, NULL);
}

static int mesh_host_runtime_apply_host_local_link(
    struct mesh_host_runtime *runtime,
    uint8_t remote_addr,
    const struct mesh_link_state_payload *payload)
{
    int rc;

    if (payload->neighbor != runtime->config.local_addr) {
        return 0;
    }

    if (payload->link_up != 0u) {
        return cluster_add_link(
            runtime->config.mesh_cluster,
            runtime->config.local_addr,
            remote_addr,
            payload->quality,
            false);
    }

    rc = cluster_remove_link(
        runtime->config.mesh_cluster,
        runtime->config.local_addr,
        remote_addr,
        false);
    if (rc == -(int)MESH_ERR_NO_ROUTE) {
        return 0;
    }

    return rc;
}

static int mesh_host_runtime_control_handler(
    void *handler_ctx,
    const struct mesh_frame_view *frame,
    uint8_t *out_reply_frame,
    size_t reply_cap,
    size_t *out_reply_len)
{
    struct mesh_host_runtime *runtime = (struct mesh_host_runtime *)handler_ctx;
    int rc;

    rc = cluster_processor_control_handler(
        runtime->config.mesh_cluster,
        frame,
        out_reply_frame,
        reply_cap,
        out_reply_len);
    if (rc != 0) {
        return rc;
    }

    switch (frame->type) {
    case MESH_TYPE_REGISTER: {
        struct mesh_register_payload payload;

        if (!mesh_parse_register(frame, &payload)) {
            return -(int)MESH_ERR_BAD_FRAME;
        }

        return mesh_host_runtime_sync_registered_node(runtime, frame->src, payload.uid);
    }

    case MESH_TYPE_LINK_STATE: {
        struct mesh_link_state_payload payload;

        if (!mesh_parse_link_state(frame, &payload)) {
            return -(int)MESH_ERR_BAD_FRAME;
        }

        /*
         * shared cluster 默认只按“src -> neighbor”记录上报方向。
         * 对主机直连接口来说，若节点明确声明“我与 host 相连”，主机就可以同步
         * 推出反向的 host -> node 可达边，否则 VFS 无法按真实 next_hop 发起请求。
         */
        rc = cluster_set_node_online(runtime->config.mesh_cluster, frame->src, true);
        if (rc != 0) {
            return rc;
        }
        mesh_host_runtime_mark_addr_online(runtime, frame->src);

        rc = mesh_host_runtime_apply_host_local_link(runtime, frame->src, &payload);
        if (rc != 0) {
            return rc;
        }

        rc = cluster_config_refresh_all_nodes_connectivity(NULL);
        if (rc != 0) {
            return rc;
        }

        return mesh_host_runtime_sync_slots_from_cluster(runtime);
    }

    case MESH_TYPE_ROUTE_UPDATE:
        rc = cluster_config_refresh_all_nodes_connectivity(NULL);
        if (rc != 0) {
            return rc;
        }
        return mesh_host_runtime_sync_slots_from_cluster(runtime);

    case MESH_TYPE_PING:
    case MESH_TYPE_PONG:
    case MESH_TYPE_TIME_SYNC:
        mesh_host_runtime_mark_addr_online(runtime, frame->src);
        return 0;

    default:
        return 0;
    }
}

void mesh_host_runtime_get_default_config(struct mesh_host_runtime_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->local_addr = 0x00u;
    out_config->default_hop = MESH_PROCESSER_DEFAULT_HOP;
}

int mesh_host_runtime_init(
    struct mesh_host_runtime *runtime,
    const struct mesh_host_runtime_config *config)
{
    struct mesh_processer_config processor_cfg;
    struct cluster *shared_cluster;
    size_t i;

    if (runtime == NULL || config == NULL || config->send_frame == NULL ||
        config->receive_frame == NULL || config->mesh_cluster == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    shared_cluster = cluster_config_mesh_cluster();
    if (shared_cluster == NULL || shared_cluster != config->mesh_cluster) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->config = *config;
    if (runtime->config.default_hop == 0u) {
        runtime->config.default_hop = MESH_PROCESSER_DEFAULT_HOP;
    }
    if (runtime->config.local_addr == MESH_ADDR_UNASSIGNED) {
        runtime->config.local_addr = runtime->config.mesh_cluster->config.local_addr;
    }

    for (i = 0u; i < MESH_HOST_RUNTIME_MAX_CLIENTS; ++i) {
        mesh_host_runtime_init_slot(runtime, i);
    }

    mesh_processer_get_default_config(&processor_cfg);
    processor_cfg.send_frame = runtime->config.send_frame;
    processor_cfg.receive_frame = runtime->config.receive_frame;
    processor_cfg.transport_ctx = runtime->config.transport_ctx;
    processor_cfg.route_lookup = cluster_processor_route_lookup;
    processor_cfg.cluster_ctx = runtime->config.mesh_cluster;
    processor_cfg.control_handler = mesh_host_runtime_control_handler;
    processor_cfg.control_handler_ctx = runtime;
    processor_cfg.local_addr = runtime->config.local_addr;
    processor_cfg.default_hop = runtime->config.default_hop;

    if (mesh_processer_init(&runtime->processor, &processor_cfg) != 0) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    runtime->next_mesh_seq = 1u;
    runtime->initialized = true;
    return 0;
}

void mesh_host_runtime_deinit(struct mesh_host_runtime *runtime)
{
    if (runtime == NULL) {
        return;
    }

    mesh_processer_deinit(&runtime->processor);
    memset(runtime, 0, sizeof(*runtime));
}

int mesh_host_runtime_process_frame(
    struct mesh_host_runtime *runtime,
    const uint8_t *frame_data,
    size_t frame_len)
{
    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return mesh_processer_process_frame(&runtime->processor, frame_data, frame_len);
}

int mesh_host_runtime_poll_once(struct mesh_host_runtime *runtime)
{
    int rc;

    if (runtime == NULL || !runtime->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = mesh_host_runtime_claim_dispatch(runtime);
    if (rc != 0) {
        return rc;
    }

    rc = mesh_processer_poll_once(&runtime->processor);
    mesh_host_runtime_release_dispatch(runtime);
    return rc;
}
