#ifndef MESH_PROCESSER_H
#define MESH_PROCESSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../envelope/mesh_protocal.h"
#include "../../mini9p/mini9p_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mesh processor（mesh 层处理器）
 *
 * 设计目标：
 * 1. 只处理 mesh envelope，不把 cluster 和 mini9P 的内部状态耦死在一起。
 * 2. 把“收帧/解帧/判断是否本机/转发/分发”统一放在一个可轮询的中间层。
 * 3. 所有和具体业务有关的动作，都通过回调注入：
 *    - route_lookup：问 cluster 下一跳。
 *    - control_handler：处理 REGISTER / ASSIGN / ROUTE_UPDATE 等控制帧。
 *    - mini9p_server_handler：处理本机收到的 mini9P T* 请求。
 *    - mini9p_client_handler：处理本机收到的 mini9P R* 响应。
 *
 * 使用方式：
 * - 如果外层已经有串口/链路轮询，就调用 mesh_processer_process_frame()。
 * - 如果希望 processor 自己拉取一帧，就调用 mesh_processer_poll_once()。
 */

/*
 * 默认转发跳数（hop / TTL 类语义）。
 *
 * 用途：
 * - 本机发起的控制帧或数据帧在没有更精细策略时，使用该默认值。
 * - 本机收到 mini9P 请求并构造响应时，也会默认使用该值重新封装。
 */
#define MESH_PROCESSER_DEFAULT_HOP 8u

/*
 * processor 内部临时缓冲区容量。
 *
 * 说明：
 * - 这里直接对齐 mesh v1 的最大 payload + 固定头，避免在 processor
 *   内部再引入额外的动态分配。
 * - 该容量同时用于接收缓存、重封装缓存与控制面回复缓存。
 */
#define MESH_PROCESSER_FRAME_CAP (MESH_MAX_PAYLOAD_LEN + MESH_FRAME_OVERHEAD)

/*
 * 原始链路发帧接口。
 *
 * 入参说明：
 * - transport_ctx：链路层上下文，通常是串口驱动、链路控制块或物理端口句柄。
 * - next_hop：processor 已经通过 cluster 查询得到的下一跳地址。
 * - tx_data / tx_len：要发出的完整 mesh 帧。
 *
 * 返回值约定：
 * - 0 表示发送成功。
 * - 负值表示链路层失败，具体错误码由底层实现决定。
 *
 * 责任边界：
 * - processor 不关心物理链路是 UART、SPI 还是别的介质。
 * - transport 也不关心帧内容，只负责把字节可靠送出去。
 */
typedef int (*mesh_processer_send_frame_fn)(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len);

/*
 * 原始链路收帧接口。
 *
 * 入参说明：
 * - transport_ctx：与 send_frame 相同的链路上下文。
 * - rx_data / rx_cap：收帧缓冲区及其容量。
 * - rx_len：输出实际收到的帧长度。
 *
 * 返回值约定：
 * - 0 表示收到了一帧完整数据。
 * - 负值表示本次轮询失败或超时。
 *
 * 建议：
 * - 如果串口没有收到完整帧，底层应返回可重试的错误码，供上层继续 poll。
 */
typedef int (*mesh_processer_receive_frame_fn)(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len);

/*
 * 路由查询接口。
 *
 * processor 在这里不维护全局拓扑，只做“问答式查路由”：
 * - out_is_local = true  表示 dst 就是本机地址。
 * - out_is_local = false 表示需要转发，并返回 out_next_hop。
 * - 若查不到路由，返回负值，processor 直接向上返回错误。
 *
 * 推荐语义：
 * - 对于直连节点，out_next_hop 可以等于 dst，也可以等于某个物理端口映射值，
 *   具体由 transport 约定。
 * - 对于中继节点，cluster 应返回真正可达的下一跳地址。
 */
typedef int (*mesh_processer_route_lookup_fn)(
    void *cluster_ctx,
    uint8_t dst,
    uint8_t *out_next_hop,
    bool *out_is_local);

/*
 * 控制面处理接口。
 *
 * processor 收到“发给本机”的控制帧后，会把该帧交给此回调。
 * 该回调的典型职责包括：
 * - REGISTER：更新节点在线状态、记录临时反向路径、发起 ASSIGN。
 * - ASSIGN：更新本机地址/节点名/租约信息。
 * - ROUTE_UPDATE：更新下一跳与路由版本。
 * - PING / TIME_SYNC：更新在线/时延统计。
 *
 * 返回值与输出约定：
 * - 0 表示处理成功。
 * - 若不需要回包，*out_reply_len = 0。
 * - 若需要回包，out_reply_frame 必须是完整的 mesh 帧，且 reply_cap 足够。
 * - 任何需要继续转发的回包，都应在回调中按自己的控制策略封装好。
 */
typedef int (*mesh_processer_control_handler_fn)(
    void *cluster_ctx,
    const struct mesh_frame_view *frame,
    uint8_t *out_reply_frame,
    size_t reply_cap,
    size_t *out_reply_len);

/*
 * 本机收到 mini9P T* 请求时调用的服务端处理接口。
 *
 * 输入：
 * - request_data / request_len：完整 mini9P 请求帧（T*）。
 * 输出：
 * - out_response_data / out_response_len：对应 mini9P 响应帧（R*）。
 *
 * 典型实现：
 * - 直接转给 mini9p_server_handle_frame()。
 * - 由 server 根据 fid / path / mode / backend 状态构造 R*。
 */
typedef int (*mesh_processer_mini9p_server_handler_fn)(
    void *server_ctx,
    const uint8_t *request_data,
    size_t request_len,
    uint8_t *out_response_data,
    size_t response_cap,
    size_t *out_response_len);

/*
 * 本机收到 mini9P R* 响应时调用的客户端侧处理接口。
 *
 * 这个回调用于把网络侧响应交回给发起请求的一侧：
 * - 由等待中的 client 状态机消化。
 * - 或者写入一个 pending 队列，由上层稍后领取。
 *
 * 注意：
 * - 该回调收到的是解码后的 mini9P 帧视图，不是 mesh envelope。
 * - processor 已经保证只有目标地址命中时才会进入该分支。
 */
typedef int (*mesh_processer_mini9p_client_handler_fn)(
    void *client_ctx,
    const struct m9p_frame_view *frame);

/*
 * mesh processor 初始化配置。
 *
 * 必填字段：
 * - send_frame / receive_frame / route_lookup
 * - transport_ctx / cluster_ctx 可按实现需要填写
 *
 * 可选字段：
 * - control_handler / mini9p_server_handler / mini9p_client_handler
 *   在某些阶段可以先置 NULL，processor 会“只分流不落地”。
 */
struct mesh_processer_config {
    mesh_processer_send_frame_fn send_frame;                /**< 原始发帧函数，不可为 NULL。 */
    mesh_processer_receive_frame_fn receive_frame;          /**< 原始收帧函数，不可为 NULL。 */
    void *transport_ctx;                                    /**< 传给 send/receive 的上下文。 */
    mesh_processer_route_lookup_fn route_lookup;            /**< 路由查询函数，不可为 NULL。 */
    void *cluster_ctx;                                      /**< 传给 cluster 回调的上下文。 */
    mesh_processer_control_handler_fn control_handler;      /**< 本机控制帧处理器，可为 NULL。 */
    void *control_handler_ctx;                              /**< 控制处理器上下文；为 NULL 时默认回退到 cluster_ctx。 */
    mesh_processer_mini9p_server_handler_fn mini9p_server_handler; /**< mini9P T* 处理器，可为 NULL。 */
    void *mini9p_server_ctx;                                /**< mini9P server 上下文。 */
    mesh_processer_mini9p_client_handler_fn mini9p_client_handler; /**< mini9P R* 处理器，可为 NULL。 */
    void *mini9p_client_ctx;                                /**< mini9P client 上下文。 */
    uint8_t local_addr;                                     /**< 本机 mesh 短地址；bootstrap 阶段可暂用 0xFF。 */
    uint8_t default_hop;                                    /**< 本机发起或回包时的默认 hop，0 表示使用默认值。 */
};

/*
 * mesh processor 运行时状态。
 *
 * 内部缓冲区说明：
 * - rx_buffer：poll_once 收到原始 mesh 帧后暂存。
 * - tx_buffer：控制回复、重封装、转发组包时暂存。
 *
 * 该结构体适合静态全局实例或外层对象成员，不需要动态分配。
 */
struct mesh_processer {
    struct mesh_processer_config config; /**< 当前生效配置。 */
    bool initialized;                    /**< 是否已初始化。 */
    uint8_t rx_buffer[MESH_PROCESSER_FRAME_CAP]; /**< 原始接收缓冲。 */
    uint8_t tx_buffer[MESH_PROCESSER_FRAME_CAP]; /**< 构帧/转发缓冲。 */
};

/*
 * 返回 processor 的默认配置模板。
 *
 * 默认值只负责把结构体清零并填入本机默认 hop 和未分配地址；
 * 真正可用的 send/receive/route_lookup 仍需调用方自己补齐。
 */
void mesh_processer_get_default_config(struct mesh_processer_config *out_config);

/*
 * 初始化 processor。
 *
 * 规则：
 * - processor 和关键回调不能为空。
 * - route_lookup 不可为空，否则 processor 无法判断是否本机或下一跳。
 * - 该函数不主动触发任何收发，只是装配运行时状态。
 */
int mesh_processer_init(struct mesh_processer *processor, const struct mesh_processer_config *config);

/*
 * 清理 processor 状态。
 *
 * 适用场景：
 * - 节点断链后重建。
 * - 上层业务决定销毁 mesh 层实例。
 */
void mesh_processer_deinit(struct mesh_processer *processor);

/*
 * 处理一帧“已经收进来”的原始 mesh 数据。
 *
 * 典型调用方式：
 * - 外层已经完成串口收帧，于是直接把帧丢给 processor。
 * - 或者在自定义链路层里先做缓存/聚合，再统一交给 processor。
 *
 * 处理流程：
 * 1. 解 mesh envelope。
 * 2. 如果不是发给自己，则按 cluster 路由转发。
 * 3. 如果是发给自己，则根据消息类型分到控制面或 mini9P。
 */
int mesh_processer_process_frame(
    struct mesh_processer *processor,
    const uint8_t *frame_data,
    size_t frame_len);

/*
 * 轮询一次：从串口/链路收一帧，然后调用 mesh_processer_process_frame()。
 *
 * 这是“processor 自己拉流”的便捷入口，适合裸循环或任务线程。
 * 若外层已经有独立收帧线程，则通常直接调用 mesh_processer_process_frame()。
 */
int mesh_processer_poll_once(struct mesh_processer *processor);

#ifdef __cplusplus
}
#endif

#endif
