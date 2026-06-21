#ifndef PWOS_SESSION_MANAGER_H
#define PWOS_SESSION_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "mini9p_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_SESSION_MANAGER_MAX_SESSIONS 32u
#define PWOS_SESSION_DEFAULT_DEADLINE_MS 1000u

/*
 * 这些错误只在主机本地传播，不会上 wire。
 * mini9P client 会原样返回 transport 的负错误码，因此这里用独立区间
 * 区分 no-route/deadline/queue-full，避免继续把所有失败伪装成 EAGAIN。
 */
#define PWOS_SESSION_ERR_NO_ROUTE   (-1001)
#define PWOS_SESSION_ERR_DEADLINE   (-1002)
#define PWOS_SESSION_ERR_QUEUE_FULL (-1003)
#define PWOS_SESSION_ERR_STALE_BOOT (-1004)

typedef struct pwos_session_manager pwos_session_manager_t;

typedef int (*pwos_session_send_fn)(
    void *ctx,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len);

typedef int (*pwos_session_wait_fn)(
    void *ctx,
    uint8_t src_addr,
    uint32_t deadline_ms,
    uint8_t *payload,
    size_t payload_cap,
    size_t *out_payload_len);

typedef struct {
    void *io_ctx;
    pwos_session_send_fn send;
    pwos_session_wait_fn wait;
    uint32_t default_deadline_ms;
} pwos_session_manager_config_t;

typedef struct {
    uint8_t used;
    uint8_t addr;
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
} pwos_session_manager_stats_t;

struct pwos_session_manager {
    pwos_session_manager_config_t config;
    pwos_session_entry_t sessions[PWOS_SESSION_MANAGER_MAX_SESSIONS];
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

struct m9p_client *pwos_session_manager_get_client(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms);

int pwos_session_manager_attach(
    pwos_session_manager_t *manager,
    uint8_t addr,
    uint32_t boot_id,
    uint32_t deadline_ms);

void pwos_session_manager_get_stats(
    const pwos_session_manager_t *manager,
    pwos_session_manager_stats_t *out_stats);

const char *pwos_session_error_name(int rc);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_SESSION_MANAGER_H */
