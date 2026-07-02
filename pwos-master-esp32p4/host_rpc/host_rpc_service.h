/* ========================================================================
 * Host RPC 服务端 —— 公共头文件
 *
 * 处理来自远端 Host 的 RPC 请求:
 *   - cluster.read_node  / cluster.write_node  (节点文件读写)
 *   - host.advertise     (主机公告)
 *   - topology.whoowns   / topology.sync        (拓扑管理)
 *
 * 设计: 依赖注入，所有业务逻辑通过 config 回调委托给 runtime 层。
 * ======================================================================== */

#ifndef PWOS_HOST_RPC_SERVICE_H
#define PWOS_HOST_RPC_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_host_rpc_methods.h"
#include "pwos_host_rpc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 内部数据缓冲区容量 ---- */
#define PWOS_HOST_RPC_SERVICE_DATA_CAP  4096u

/* ========================================================================
 * 第一节: 回调类型 (由 runtime 层实现)
 * ======================================================================== */

typedef int (*pwos_host_rpc_service_read_fn)(
    void *ctx, const char *target, const char *path,
    uint8_t *data, uint16_t *in_out_len, uint32_t deadline_ms);

typedef int (*pwos_host_rpc_service_write_fn)(
    void *ctx, const char *target, const char *path,
    const uint8_t *data, uint16_t data_len,
    uint16_t *out_written, uint32_t deadline_ms);

typedef int (*pwos_host_rpc_service_advertise_fn)(
    void *ctx, const pwos_host_rpc_advertise_t *advertise);

typedef int (*pwos_host_rpc_service_local_advertise_fn)(
    void *ctx, pwos_host_rpc_advertise_t *out_advertise);

typedef int (*pwos_host_rpc_service_whoowns_fn)(
    void *ctx, const char *target,
    pwos_host_rpc_advertise_t *out_owner);

typedef int (*pwos_host_rpc_service_topology_sync_fn)(
    void *ctx,
    const pwos_host_rpc_topology_t *incoming,
    pwos_host_rpc_topology_t        *out_current);

/* ========================================================================
 * 第二节: 配置 & 统计 & 服务结构
 * ======================================================================== */

typedef struct {
    void                                  *ctx;
    pwos_host_rpc_service_read_fn          read_node;
    pwos_host_rpc_service_write_fn         write_node;
    pwos_host_rpc_service_advertise_fn     advertise;
    pwos_host_rpc_service_local_advertise_fn local_advertise;
    pwos_host_rpc_service_whoowns_fn       whoowns;
    pwos_host_rpc_service_topology_sync_fn topology_sync;
} pwos_host_rpc_service_config_t;

typedef struct {
    uint32_t requests;
    uint32_t responses;
    uint32_t bad_frames;
    uint32_t not_found;
    uint32_t remote_errors;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t advertise_calls;
    uint32_t whoowns_calls;
    uint32_t topology_sync_calls;
    uint32_t last_call_id;
    uint16_t last_status;
} pwos_host_rpc_service_stats_t;

typedef struct {
    pwos_host_rpc_service_config_t config;
    pwos_host_rpc_service_stats_t  stats;
    uint8_t data[PWOS_HOST_RPC_SERVICE_DATA_CAP];      /* 读/写临时缓冲 */
    uint8_t method_payload[PWOS_HOST_RPC_MAX_PAYLOAD_LEN]; /* 方法响应缓冲 */
    pwos_host_rpc_topology_t incoming_topology;         /* 入站拓扑 */
    pwos_host_rpc_topology_t outgoing_topology;         /* 出站拓扑 */
} pwos_host_rpc_service_t;

/* ========================================================================
 * 第三节: API
 * ======================================================================== */

int pwos_host_rpc_service_init(
    pwos_host_rpc_service_t              *service,
    const pwos_host_rpc_service_config_t *config);

/**
 * 处理一个 RPC 请求帧，生成响应帧。
 * 自动路由到对应的 dispatch 函数 (service.method 匹配)。
 */
int pwos_host_rpc_service_handle(
    pwos_host_rpc_service_t *service,
    const uint8_t *request_frame,  size_t request_len,
    uint8_t       *response_frame, size_t response_cap,
    size_t        *out_response_len);

void pwos_host_rpc_service_get_stats(
    const pwos_host_rpc_service_t       *service,
    pwos_host_rpc_service_stats_t       *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_SERVICE_H */
