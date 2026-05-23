#include "mesh_processer.h"

#include <string.h>

/*
 * 实现说明：
 *
 * 这个文件只负责 mesh 层处理器本身，不把具体 cluster / mini9P 的实现细节
 * 拉进来。它的工作是：
 * - 从外界拿到一帧 mesh 数据；
 * - 判断该帧是给自己还是需要转发；
 * - 如果是控制帧，把它交给 cluster 回调；
 * - 如果是 mini9P 帧，把 T* 交给 server 回调，把 R* 交给 client 回调；
 * - 必要时把回应重新封成 mesh 帧并发出去。
 *
 * 这样上层可以把 transport、cluster、client/server 逐步接入，而不需要先
 * 设计一个巨大的耦合对象。
 */

static uint8_t mesh_processer_default_hop_value(const struct mesh_processer *processor)
{
    if (processor == NULL || processor->config.default_hop == 0u) {
        return MESH_PROCESSER_DEFAULT_HOP;
    }
    return processor->config.default_hop;
}

static bool mesh_processer_is_request_type(uint8_t type)
{
    /* mini9P 约定：响应类型的最高位为 1，请求类型最高位为 0。 */
    return (type & 0x80u) == 0u;
}

/*
 * 正常情况下，“本机命中”由 dst == local_addr 判定。
 *
 * 但 REGISTER 是 bootstrap 特例：
 * - 协议 helper 会把 REGISTER 的 dst 固定编码成 MESH_ADDR_UNASSIGNED；
 * - 主机必须把这类帧收进控制面，否则真正的 runtime 无法通过 REGISTER
 *   自动发现新节点。
 */
static bool mesh_processer_targets_local(
    const struct mesh_processer *processor,
    const struct mesh_frame_view *frame)
{
    if (processor == NULL || frame == NULL) {
        return false;
    }
    if (frame->dst == processor->config.local_addr) {
        return true;
    }

    return frame->type == MESH_TYPE_REGISTER && frame->dst == MESH_ADDR_UNASSIGNED;
}

/*
 * 询问 cluster：某个 dst 是否为本机，以及需要发往哪个下一跳。
 *
 * 这个 helper 不做任何缓存，也不维护路由表；它只是把查询动作集中起来，
 * 方便上层以后替换 route_lookup 逻辑。
 */
static int mesh_processer_lookup_route(
    struct mesh_processer *processor,
    uint8_t dst,
    uint8_t *out_next_hop,
    bool *out_is_local)
{
    if (processor == NULL || !processor->initialized || processor->config.route_lookup == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return processor->config.route_lookup(
        processor->config.cluster_ctx,
        dst,
        out_next_hop,
        out_is_local);
}

/*
 * 发出一帧已经编码好的 mesh 数据。
 *
 * 使用场景：
 * - 控制面回包已经由 control_handler 组装完成。
 * - mini9P server 已经生成响应，processor 需要把它再封进 mesh envelope。
 */
static int mesh_processer_send_raw_frame(
    struct mesh_processer *processor,
    const uint8_t *frame_data,
    size_t frame_len)
{
    struct mesh_frame_view frame;
    uint8_t next_hop = 0u;
    bool is_local = false;
    int rc;

    if (processor == NULL || !processor->initialized || frame_data == NULL || frame_len == 0u) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (!mesh_decode_frame(frame_data, frame_len, &frame)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    /* 如果这帧不是发给本机，那么先问 cluster 再转发。 */
    rc = mesh_processer_lookup_route(processor, frame.dst, &next_hop, &is_local);
    if (rc != 0) {
        return rc;
    }
    if (is_local) {
        return 0;
    }

    return processor->config.send_frame(
        processor->config.transport_ctx,
        next_hop,
        frame_data,
        frame_len);
}

/*
 * 对“已经解码过的 mesh 帧”做转发。
 *
 * 与 mesh_processer_send_raw_frame() 的区别：
 * - 这里拿到的是解析后的 frame view，因此可以修改 hop 后重新封装。
 * - 适合转发路径，尤其是 hop 递减这一步必须做。
 */
static int mesh_processer_forward_decoded_frame(
    struct mesh_processer *processor,
    const struct mesh_frame_view *frame)
{
    uint8_t next_hop = 0u;
    bool is_local = false;
    uint8_t forward_hop;
    size_t tx_len = 0u;
    int rc;

    if (processor == NULL || frame == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (frame->hop == 0u) {
        return -(int)MESH_ERR_NO_ROUTE;
    }

    rc = mesh_processer_lookup_route(processor, frame->dst, &next_hop, &is_local);
    if (rc != 0) {
        return rc;
    }
    if (is_local) {
        return 0;
    }

    forward_hop = (uint8_t)(frame->hop - 1u);
    if (!mesh_encode_frame(
            frame->type,
            frame->src,
            frame->dst,
            frame->seq,
            forward_hop,
            frame->flags,
            frame->payload,
            frame->payload_len,
            processor->tx_buffer,
            sizeof(processor->tx_buffer),
            &tx_len)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    return processor->config.send_frame(
        processor->config.transport_ctx,
        next_hop,
        processor->tx_buffer,
        tx_len);
}

/*
 * 处理“发给本机”的控制帧。
 *
 * 这一层不理解 REGISTER/ASSIGN/ROUTE_UPDATE 的业务细节，只把帧交给
 * control_handler。control_handler 如果需要回包，直接返回完整 mesh 帧，
 * processor 只负责把回包发出去。
 */
static int mesh_processer_dispatch_control(
    struct mesh_processer *processor,
    const struct mesh_frame_view *frame)
{
    void *handler_ctx;
    size_t reply_len = 0u;
    int rc;

    if (processor == NULL || frame == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    if (processor->config.control_handler == NULL) {
        /*
         * 暂未接入 cluster 时：
         * - 不报错；
         * - 只是不做进一步业务处理。
         * 这样 processor 可以先独立跑通链路层。
         */
        return 0;
    }

    handler_ctx = processor->config.control_handler_ctx;
    if (handler_ctx == NULL) {
        handler_ctx = processor->config.cluster_ctx;
    }

    rc = processor->config.control_handler(
        handler_ctx,
        frame,
        processor->tx_buffer,
        sizeof(processor->tx_buffer),
        &reply_len);
    if (rc != 0) {
        return rc;
    }
    if (reply_len == 0u) {
        return 0;
    }

    /* 回包已经是完整 mesh 帧，直接交给链路层发出。 */
    return mesh_processer_send_raw_frame(processor, processor->tx_buffer, reply_len);
}

/*
 * 处理“发给本机”的 mini9P 帧。
 *
 * - 如果 payload 里是 T*，就交给 mini9p_server_handler。
 * - 如果 payload 里是 R*，就交给 mini9p_client_handler。
 *
 * 注意：这里做的是“协议方向分流”，不是业务实现。
 */
static int mesh_processer_dispatch_mini9p(
    struct mesh_processer *processor,
    const struct mesh_frame_view *frame)
{
    const struct m9p_frame_view *mini9p_view;
    struct m9p_frame_view view;
    size_t response_len = 0u;
    int rc;

    if (processor == NULL || frame == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (!m9p_decode_frame(frame->payload, frame->payload_len, &view)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    mini9p_view = &view;

    if (mesh_processer_is_request_type(mini9p_view->type)) {
        if (processor->config.mini9p_server_handler == NULL) {
            /* 暂未接入 server：先吃掉请求，避免阻塞总线。 */
            return 0;
        }

        rc = processor->config.mini9p_server_handler(
            processor->config.mini9p_server_ctx,
            frame->payload,
            frame->payload_len,
            processor->tx_buffer,
            sizeof(processor->tx_buffer),
            &response_len);
        if (rc != 0) {
            return rc;
        }
        if (response_len == 0u) {
            return 0;
        }

        /*
         * mini9P server 产出的响应还是 mini9P 帧，需要再封装进 mesh 数据面。
         * 这里使用原始请求源地址作为回包目标地址。
         */
        if (!mesh_build_mini9p_frame(
                processor->config.local_addr,
                frame->src,
                frame->seq,
                mesh_processer_default_hop_value(processor),
                0u,
                processor->tx_buffer,
                (uint16_t)response_len,
                processor->rx_buffer,
                sizeof(processor->rx_buffer),
                &response_len)) {
            return -(int)MESH_ERR_BAD_FRAME;
        }

        return mesh_processer_send_raw_frame(processor, processor->rx_buffer, response_len);
    }

    if (processor->config.mini9p_client_handler != NULL) {
        /*
         * R* 响应只交给客户端侧处理器，不在 processor 内部解释其业务含义。
         */
        return processor->config.mini9p_client_handler(
            processor->config.mini9p_client_ctx,
            mini9p_view);
    }

    /* 暂未接入 client：先吞掉响应，保持 processor 自身稳定。 */
    return 0;
}

/*
 * 填充默认配置。
 *
 * 默认只提供最小安全值：
 * - local_addr 使用未分配地址；
 * - default_hop 使用固定默认跳数；
 * - 其余回调清零，要求调用方显式装配。
 */
void mesh_processer_get_default_config(struct mesh_processer_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->default_hop = MESH_PROCESSER_DEFAULT_HOP;
    out_config->local_addr = MESH_ADDR_UNASSIGNED;
}

int mesh_processer_init(struct mesh_processer *processor, const struct mesh_processer_config *config)
{
    struct mesh_processer_config default_config;

    if (processor == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    memset(processor, 0, sizeof(*processor));
    mesh_processer_get_default_config(&default_config);
    if (config != NULL) {
        default_config = *config;
    }
    if (default_config.send_frame == NULL || default_config.receive_frame == NULL ||
        default_config.route_lookup == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (default_config.default_hop == 0u) {
        default_config.default_hop = MESH_PROCESSER_DEFAULT_HOP;
    }

    processor->config = default_config;
    processor->initialized = true;
    return 0;
}

/* 反初始化：把内部状态清零，适合链路断开后整体重建。 */
void mesh_processer_deinit(struct mesh_processer *processor)
{
    if (processor == NULL) {
        return;
    }

    memset(processor, 0, sizeof(*processor));
}

int mesh_processer_process_frame(
    struct mesh_processer *processor,
    const uint8_t *frame_data,
    size_t frame_len)
{
    struct mesh_frame_view frame;

    if (processor == NULL || !processor->initialized || frame_data == NULL) {
        return -(int)MESH_ERR_BAD_FRAME;
    }
    if (!mesh_decode_frame(frame_data, frame_len, &frame)) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    /* 非本机目标：按 cluster 查询下一跳后转发。 */
    if (!mesh_processer_targets_local(processor, &frame)) {
        return mesh_processer_forward_decoded_frame(processor, &frame);
    }

    /* 本机目标：控制面和数据面分开处理。 */
    if (mesh_is_control_type(frame.type)) {
        return mesh_processer_dispatch_control(processor, &frame);
    }

    if (frame.type == MESH_TYPE_MINI9P) {
        return mesh_processer_dispatch_mini9p(processor, &frame);
    }

    return -(int)MESH_ERR_UNSUPPORTED_TYPE;
}

/*
 * 轮询一次：从底层链路取一帧，再交给统一处理入口。
 *
 * 这是最适合放在任务循环里的接口：
 * while (1) { mesh_processer_poll_once(&processor); }
 */
int mesh_processer_poll_once(struct mesh_processer *processor)
{
    size_t rx_len = 0u;
    int rc;

    if (processor == NULL || !processor->initialized) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    rc = processor->config.receive_frame(
        processor->config.transport_ctx,
        processor->rx_buffer,
        sizeof(processor->rx_buffer),
        &rx_len);
    if (rc != 0) {
        return rc;
    }

    return mesh_processer_process_frame(processor, processor->rx_buffer, rx_len);
}
