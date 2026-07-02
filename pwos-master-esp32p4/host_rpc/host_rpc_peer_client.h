/* ========================================================================
 * Host RPC Peer 客户端 —— 公共头文件
 *
 * 封装与远端 Host 之间的 RPC 通信:
 *   - TCP 帧交换 (通过 exchange 回调)
 *   - 通用 RPC call (服务/方法路由)
 *   - 节点文件读写 (read_node / write_node)
 * ======================================================================== */

#ifndef PWOS_HOST_RPC_PEER_CLIENT_H
#define PWOS_HOST_RPC_PEER_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_host_rpc_methods.h"
#include "pwos_host_rpc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 远程错误码基数 (本地错误码 + 此值 = 远程错误) ---- */
#define PWOS_HOST_RPC_REMOTE_ERROR_BASE  2000

/* ========================================================================
 * 第一节: 回调类型
 * ======================================================================== */

/**
 * TCP 帧交换回调: 发送 request，阻塞等待 response。
 * @param out_response_len  输出: 实际收到的响应长度
 */
typedef int (*pwos_host_rpc_exchange_fn)(
    void          *ctx,
    const uint8_t *request,  size_t request_len,
    uint8_t       *response, size_t response_cap,
    size_t        *out_response_len,
    uint32_t       deadline_ms);

/** 客户端锁回调 (用于线程安全，可为 NULL 表示单线程) */
typedef void (*pwos_host_rpc_client_lock_fn)(void *ctx);

/* ========================================================================
 * 第二节: 配置 & 统计
 * ======================================================================== */

typedef struct {
    void                         *io_ctx;      /* exchange 回调上下文      */
    pwos_host_rpc_exchange_fn     exchange;    /* TCP 帧交换回调           */
    void                         *lock_ctx;    /* 锁回调上下文             */
    pwos_host_rpc_client_lock_fn  lock;        /* 加锁回调 (可为 NULL)     */
    pwos_host_rpc_client_lock_fn  unlock;      /* 解锁回调 (可为 NULL)     */
} pwos_host_rpc_peer_client_config_t;

typedef struct {
    uint32_t calls;              /* 累计 RPC 调用次数        */
    uint32_t responses;          /* 累计成功响应次数          */
    uint32_t transport_errors;   /* 传输层错误次数            */
    uint32_t remote_errors;      /* 远端返回错误次数          */
    uint32_t malformed_responses;/* 响应格式错误次数          */
    uint32_t last_call_id;       /* 最近 call_id             */
    uint16_t last_status;        /* 最近远端状态码            */
    int32_t  last_error;         /* 最近错误码               */
} pwos_host_rpc_peer_client_stats_t;

/* ========================================================================
 * 第三节: 客户端结构 & API
 * ======================================================================== */

typedef struct {
    pwos_host_rpc_peer_client_config_t config;     /* 配置副本            */
    uint32_t                           next_call_id; /* 自增 call_id       */
    pwos_host_rpc_peer_client_stats_t  stats;       /* 运行统计            */
} pwos_host_rpc_peer_client_t;

/**
 * 初始化客户端。
 * lock/unlock 必须成对提供或都为 NULL。
 */
int pwos_host_rpc_peer_client_init(
    pwos_host_rpc_peer_client_t              *client,
    const pwos_host_rpc_peer_client_config_t *config);

/**
 * 通用 RPC 调用: 编码 → exchange → 解码响应。
 */
int pwos_host_rpc_peer_client_call(
    pwos_host_rpc_peer_client_t *client,
    const char *service, const char *method,
    const uint8_t *payload, uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t  *response_payload, uint16_t *in_out_response_len,
    uint16_t *out_status);

/**
 * 读取远端节点文件 (封装 cluster.read_node)。
 */
int pwos_host_rpc_peer_client_read_node(
    pwos_host_rpc_peer_client_t *client,
    const char *target, const char *path,
    uint8_t *data, uint16_t *in_out_len,
    uint32_t deadline_ms);

/**
 * 写入远端节点文件 (封装 cluster.write_node)。
 */
int pwos_host_rpc_peer_client_write_node(
    pwos_host_rpc_peer_client_t *client,
    const char *target, const char *path,
    const uint8_t *data, uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms);

/** 获取统计快照 (线程安全) */
void pwos_host_rpc_peer_client_get_stats(
    pwos_host_rpc_peer_client_t       *client,
    pwos_host_rpc_peer_client_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_PEER_CLIENT_H */
