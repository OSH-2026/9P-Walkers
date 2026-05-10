/**
 * @file mini9p_peer_link.h
 * @brief Mini9P 双向对等链路分发层。
 *
 * 本模块位于 raw frame transport 和 mini9p_server 之间，解决这样一个问题：
 * 同一条 UART 上，双方都可能在任意时刻主动发起 T* 请求。
 *
 * 仅靠 send_frame/receive_frame 这样的原始收发接口，调用方只能“发完一帧，等下一帧”，
 * 但在等待自己 R* 响应的过程中，链路上可能先到的是对端主动发来的 T* 请求。
 *
 * m9p_peer_link 的职责就是在这一层做分发：
 * 1. 本端发起请求时，持续接收后续帧。
 * 2. 如果收到匹配本端 tag 的 R* 响应，则返回给本端请求者。
 * 3. 如果收到对端主动发来的 T* 请求，则交给 request_handler 处理并回发响应。
 * 4. 如果收到不属于本端当前请求的“外来帧”，则交给可选的 foreign_frame_handler。
 *
 * 这样就能在不改 mini9p_client / mini9p_server 外部签名的前提下，支持同一条链路上的
 * 双向主动请求。
 */

#ifndef MINI9P_PEER_LINK_H
#define MINI9P_PEER_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 发送一整帧原始数据。
 *
 * 典型实现直接包装 UART transport 的 send_frame。
 */
typedef int (*m9p_peer_link_send_frame_fn)(
    void *transport_ctx,
    const uint8_t *tx_data,
    size_t tx_len);

/**
 * @brief 接收一整帧原始数据。
 *
 * 典型实现直接包装 UART transport 的 receive_frame。
 */
typedef int (*m9p_peer_link_receive_frame_fn)(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len);

/**
 * @brief 处理一帧对端主动发来的 T* 请求，并生成对应响应。
 *
 * 这个签名刻意与 m9p_server_handle_frame 保持一致，便于直接把 mini9p_server
 * 挂到 peer_link 下方。
 */
typedef int (*m9p_peer_link_request_handler_fn)(
    void *handler_ctx,
    const uint8_t *request_data,
    size_t request_len,
    uint8_t *response_data,
    size_t response_cap,
    size_t *response_len);

/**
 * @brief 观察并处理“非本端当前等待目标”的外来帧。
 *
 * 当前主要用途是：
 * 1. 记录或转发不匹配当前 pending tag 的 R* 响应。
 * 2. 为后续更复杂的路由/代理层预留接管入口。
 *
 * 若未提供该回调，peer_link 会直接忽略这类外来响应帧。
 */
typedef int (*m9p_peer_link_foreign_frame_handler_fn)(
    void *handler_ctx,
    const uint8_t *frame_data,
    size_t frame_len,
    const struct m9p_frame_view *frame);

/**
 * @brief peer_link 初始化配置。
 */
struct m9p_peer_link_config {
    m9p_peer_link_send_frame_fn send_frame;                  /**< 原始发帧函数。 */
    m9p_peer_link_receive_frame_fn receive_frame;            /**< 原始收帧函数。 */
    void *transport_ctx;                                     /**< 传给 raw transport 的上下文。 */
    m9p_peer_link_request_handler_fn request_handler;        /**< 对端主动请求的处理器，可为 NULL。 */
    void *request_handler_ctx;                               /**< 传给 request_handler 的上下文。 */
    m9p_peer_link_foreign_frame_handler_fn foreign_frame_handler; /**< 非目标外来帧观察器，可为 NULL。 */
    void *foreign_frame_handler_ctx;                         /**< 传给 foreign_frame_handler 的上下文。 */
    uint8_t *dispatch_rx_buffer;                             /**< 内部分发用接收缓冲区。 */
    size_t dispatch_rx_cap;                                  /**< dispatch_rx_buffer 容量。 */
    uint8_t *dispatch_tx_buffer;                             /**< 处理外来请求时构造响应所用缓冲区。 */
    size_t dispatch_tx_cap;                                  /**< dispatch_tx_buffer 容量。 */
};

/**
 * @brief peer_link 运行时状态。
 */
struct m9p_peer_link {
    struct m9p_peer_link_config config;  /**< 当前生效配置。 */
    bool initialized;                    /**< 是否已初始化。 */
    bool dispatch_busy;                  /**< 是否已有调用方占用链路分发权。 */
};

/**
 * @brief 用零值填充默认配置。
 *
 * 由于本模块必须绑定具体 raw transport 和缓冲区，默认配置只负责清零，
 * 调用方仍需补齐 send/receive/buffer 等关键字段。
 */
void m9p_peer_link_get_default_config(struct m9p_peer_link_config *out_config);

/**
 * @brief 初始化 peer_link。
 *
 * @param[out] link   待初始化实例。
 * @param[in]  config 初始化配置。
 * @return 0 成功；负 Mini9P 错误码失败。
 */
int m9p_peer_link_init(struct m9p_peer_link *link, const struct m9p_peer_link_config *config);

/**
 * @brief 反初始化 peer_link。
 */
void m9p_peer_link_deinit(struct m9p_peer_link *link);

/**
 * @brief 作为 m9p_client transport_fn 使用的请求入口。
 *
 * 该接口会在等待本端响应时继续消费链路：
 * - 收到匹配本端 tag 的 R* 时返回。
 * - 收到对端主动发来的 T* 时，调用 request_handler 并回发响应后继续等待。
 * - 收到其他外来 R* 时，交给 foreign_frame_handler（若存在）后继续等待。
 */
int m9p_peer_link_request(
    void *link_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len);

/**
 * @brief 在本端空闲时处理至多一帧外来请求。
 *
 * 该接口通常用于 idle/主循环：
 * - 收到 T* 时：调用 request_handler 并回发响应。
 * - 收到非目标 R* 时：交给 foreign_frame_handler，或返回 -M9P_ERR_ETAG。
 */
int m9p_peer_link_poll_once(struct m9p_peer_link *link);

#ifdef __cplusplus
}
#endif

#endif