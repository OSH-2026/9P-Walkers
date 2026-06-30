#ifndef PWOS_SESSION_MANAGER_H
#define PWOS_SESSION_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "mini9p_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_SESSION_MANAGER_MAX_SESSIONS 32u
#define PWOS_SESSION_MANAGER_MAX_PENDING 8u
#define PWOS_SESSION_DEFAULT_DEADLINE_MS 1000u

/* 这些错误只在主机本地传播，不会上 wire。 */
#define PWOS_SESSION_ERR_NO_ROUTE   (-1001)
#define PWOS_SESSION_ERR_DEADLINE   (-1002)
#define PWOS_SESSION_ERR_QUEUE_FULL (-1003)
#define PWOS_SESSION_ERR_STALE_BOOT (-1004)

typedef struct pwos_session_manager pwos_session_manager_t;

typedef int (*pwos_session_send_fn)(
    void *ctx,
    uint8_t data_type,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len);

typedef int (*pwos_session_retag_fn)(
    uint8_t *frame,
    size_t frame_len,
    uint16_t wire_tag);

typedef void (*pwos_session_lock_fn)(void *ctx);
typedef void (*pwos_session_client_lock_fn)(void *ctx, uint8_t session_index);
typedef void (*pwos_session_pending_reset_fn)(void *ctx, uint8_t pending_index);
typedef int (*pwos_session_pending_wait_fn)(
    void *ctx,
    uint8_t pending_index,
    uint32_t timeout_ms);
typedef void (*pwos_session_pending_signal_fn)(void *ctx, uint8_t pending_index);
typedef uint32_t (*pwos_session_now_ms_fn)(void *ctx);

typedef struct {
    void *io_ctx;
    pwos_session_send_fn send;
    uint32_t default_deadline_ms;

    /*
     * 同步回调由目标平台提供。PC 单测可留空并在 send 中同步投递响应；
     * FreeRTOS 运行时使用 mutex + binary semaphore 实现真正的并发等待。
     */
    void *sync_ctx;
    pwos_session_lock_fn lock;
    pwos_session_lock_fn unlock;
    pwos_session_client_lock_fn client_lock;
    pwos_session_client_lock_fn client_unlock;
    pwos_session_pending_reset_fn pending_reset;
    pwos_session_pending_wait_fn pending_wait;
    pwos_session_pending_signal_fn pending_signal;
    pwos_session_now_ms_fn now_ms;
} pwos_session_manager_config_t;

typedef struct {
    uint8_t used;
    uint8_t resetting;
    uint8_t addr;
    uint8_t index;
    uint32_t boot_id;
    uint32_t deadline_ms;
    uint8_t attached;
    uint16_t last_tx_tag;
    uint16_t last_rx_tag;
    uint8_t last_tx_type;
    uint8_t last_rx_type;
    uint32_t tx_requests;
    uint32_t rx_responses;
    uint32_t attach_ok;
    uint32_t attach_fail;
    uint32_t deadline_errors;
    uint32_t no_route_errors;
    uint32_t queue_full_errors;
    uint32_t io_errors;
    struct m9p_client client;
    pwos_session_manager_t *manager;
} pwos_session_entry_t;

typedef struct {
    uint8_t used;
    uint8_t completed;
    uint8_t streaming;
    uint8_t data_type;
    uint8_t src_addr;
    uint8_t session_index;
    uint16_t wire_tag;
    uint32_t boot_id;
    int32_t status;
    uint16_t remote_status;
    uint16_t stream_parts;
    uint16_t response_len;
    uint8_t response[M9P_CLIENT_BUFFER_CAP];
} pwos_session_pending_t;

typedef struct {
    uint32_t tx_requests;
    uint32_t rx_responses;
    uint32_t attach_ok;
    uint32_t attach_fail;
    uint32_t resets;
    uint32_t deadline_errors;
    uint32_t no_route_errors;
    uint32_t queue_full_errors;
    uint32_t stale_boot_errors;
    uint32_t io_errors;
    uint32_t pending_delivered;
    uint32_t pending_unmatched;
    uint32_t pending_malformed;
    uint32_t pending_cancelled;
    uint32_t pending_peak;
    uint32_t stream_parts_delivered;
    uint32_t stream_completed;
} pwos_session_manager_stats_t;

typedef struct {
    uint8_t used;
    uint8_t resetting;
    uint8_t addr;
    uint8_t attached;
    uint32_t boot_id;
    uint32_t deadline_ms;
    uint16_t last_tx_tag;
    uint16_t last_rx_tag;
    uint8_t last_tx_type;
    uint8_t last_rx_type;
    uint32_t tx_requests;
    uint32_t rx_responses;
    uint32_t deadline_errors;
    uint32_t no_route_errors;
    uint32_t queue_full_errors;
    uint32_t io_errors;
} pwos_session_snapshot_t;

struct pwos_session_manager {
    pwos_session_manager_config_t config;
    pwos_session_entry_t sessions[PWOS_SESSION_MANAGER_MAX_SESSIONS];
    pwos_session_pending_t pending[PWOS_SESSION_MANAGER_MAX_PENDING];
    uint16_t next_wire_tag;
    pwos_session_manager_stats_t stats;
};

int pwos_session_manager_init(
    pwos_session_manager_t *manager,
    const pwos_session_manager_config_t *config);

int pwos_session_manager_update_node(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id);

void pwos_session_manager_reset_node(pwos_session_manager_t *manager, uint8_t addr);

/*
 * acquire/release 必须成对调用。锁只串行化同一节点的 mini9P client，
 * 不同节点使用不同 session_index，因此仍可并发。
 */
int pwos_session_manager_acquire_client(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms,
    struct m9p_client **out_client,
    uint8_t *out_session_index);

void pwos_session_manager_release_client(
    pwos_session_manager_t *manager,
    uint8_t session_index);

int pwos_session_manager_attach(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms);

/* 仅由唯一 RX consumer 调用，按 (src_addr, wire_tag) 投递响应。 */
int pwos_session_manager_deliver_mini9p(
    pwos_session_manager_t *manager,
    uint8_t src_addr,
    const uint8_t *payload,
    size_t payload_len);

/*
 * 为 mini9P 之外的数据协议执行一次 request/response。
 * pending 使用 (data_type, src_addr, wire_tag) 匹配，允许同节点不同协议并发。
 */
int pwos_session_manager_request_data(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *request,
    size_t request_len,
    pwos_session_retag_fn retag,
    uint32_t deadline_ms,
    uint8_t *response,
    size_t response_cap,
    size_t *response_len,
    uint16_t *out_wire_tag);

/*
 * 发起流式请求。每个 part 由唯一 RX consumer 追加到固定 pending 缓冲，
 * 只有 final part 才唤醒等待者并返回远端状态。
 */
int pwos_session_manager_request_stream_data(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *request,
    size_t request_len,
    pwos_session_retag_fn retag,
    uint32_t deadline_ms,
    uint8_t *response,
    size_t response_cap,
    size_t *response_len,
    uint16_t *out_wire_tag,
    uint16_t *out_remote_status,
    uint16_t *out_part_count);

/* 发送无需响应的数据帧，用于 RPC cancel / fire-and-forget。 */
int pwos_session_manager_send_data(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *payload,
    uint16_t payload_len);

int pwos_session_manager_send_oneway_data(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint8_t data_type,
    const uint8_t *payload,
    size_t payload_len,
    pwos_session_retag_fn retag,
    uint16_t *out_wire_tag);

int pwos_session_manager_deliver_data(
    pwos_session_manager_t *manager,
    uint8_t data_type,
    uint8_t src_addr,
    uint16_t wire_tag,
    const uint8_t *payload,
    size_t payload_len);

int pwos_session_manager_deliver_data_part(
    pwos_session_manager_t *manager,
    uint8_t data_type,
    uint8_t src_addr,
    uint16_t wire_tag,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t final,
    uint16_t status_or_part_index);

void pwos_session_manager_get_stats(
    const pwos_session_manager_t *manager,
    pwos_session_manager_stats_t *out_stats);

int pwos_session_manager_get_snapshot(
    pwos_session_manager_t *manager,
    size_t index,
    pwos_session_snapshot_t *out_snapshot);

const char *pwos_session_error_name(int rc);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_SESSION_MANAGER_H */
