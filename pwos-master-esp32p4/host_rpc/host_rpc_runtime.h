/* ========================================================================
 * Host RPC 运行时 —— 公共头文件
 *
 * 多主机协同核心:
 *   - TCP Server:  监听 9909 端口，处理远端请求
 *   - mDNS 发现:   自动发现局域网内其他 PWOS 主机
 *   - 选举协议:      Leader/Follower 角色协商
 *   - 拓扑同步:     全局 MCU 命名空间 & 路由表
 *   - 分布式推理:   llm_submit / result / status 跨主机代理
 * ======================================================================== */

#ifndef PWOS_HOST_RPC_RUNTIME_H
#define PWOS_HOST_RPC_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_host_election.h"
#include "pwos_host_rpc_methods.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 网络配置 ---- */
#define PWOS_HOST_RPC_PORT      9909u   /* TCP 监听端口                    */

/* ---- 容量限制 ---- */
#define PWOS_HOST_RPC_MAX_PEERS PWOS_HOST_ELECTION_MAX_PEERS  /* 最大对等主机数 */

/* ========================================================================
 * 第一节: 对等主机快照 (Peer Snapshot)
 * ======================================================================== */

typedef struct {
    uint8_t  used;                                       /* 槽位是否占用      */
    uint8_t  role;                                       /* Leader/Follower   */
    uint32_t uid[3];                                     /* 主机唯一 ID       */
    uint32_t epoch;                                      /* 选举纪元          */
    uint16_t priority;                                   /* 选举优先级        */
    uint16_t port;                                       /* RPC 端口          */
    uint32_t last_seen_ms;                               /* 最后活跃时间戳     */
    char     hostname[PWOS_HOST_RPC_HOSTNAME_CAP];       /* 主机名            */
    char     ip[16];                                     /* IP 地址           */
} pwos_host_rpc_peer_snapshot_t;

/* ========================================================================
 * 第二节: 运行时状态 (诊断用)
 * ======================================================================== */

typedef struct {
    uint8_t  initialized;         /* 是否已启动                            */
    uint8_t  server_started;      /* TCP Server 是否监听中                  */
    uint8_t  discovery_started;   /* mDNS 发现任务是否运行                   */
    uint8_t  local_role;          /* 本地角色 (LEADER/FOLLOWER)             */
    uint8_t  peer_count;          /* 当前在线 peer 数                       */
    uint8_t  topology_nodes;      /* 拓扑表节点数                           */
    uint32_t local_uid[3];        /* 本地 UID                              */
    uint32_t local_epoch;         /* 本地 epoch                            */
    uint16_t local_priority;      /* 本地选举优先级                          */
    uint32_t leader_uid[3];       /* 当前 Leader UID                       */
    /* ---- 统计计数器 ---- */
    uint32_t accepted;            /* TCP accept 次数                        */
    uint32_t server_requests;     /* Server 处理请求数                       */
    uint32_t server_errors;       /* Server 错误数                          */
    uint32_t discovery_queries;   /* mDNS 查询次数                          */
    uint32_t discovery_results;   /* mDNS 结果数                            */
    uint32_t advertise_ok;        /* 公告成功次数                            */
    uint32_t advertise_fail;      /* 公告失败次数                            */
    uint32_t topology_sync_ok;    /* 拓扑同步成功次数                         */
    uint32_t topology_sync_fail;  /* 拓扑同步失败次数                         */
    uint32_t client_calls;        /* 客户端 RPC 调用次数                     */
    uint32_t client_errors;       /* 客户端错误次数                          */
    uint32_t remote_reads;        /* 远端读取次数                            */
    uint32_t remote_writes;       /* 远端写入次数                            */
    int32_t  last_error;          /* 最近错误码                              */
    char     hostname[PWOS_HOST_RPC_HOSTNAME_CAP];  /* 本机主机名             */
} pwos_host_rpc_runtime_status_t;

/* ========================================================================
 * 第三节: 公开 API
 * ======================================================================== */

/** 启动 Host RPC 运行时 (需 LAN 已就绪) */
int pwos_host_rpc_runtime_start(void);

/** 获取运行时状态快照 (线程安全) */
void pwos_host_rpc_runtime_get_status(
    pwos_host_rpc_runtime_status_t *out_status);

/** 获取第 index 个 peer 的快照 */
int pwos_host_rpc_runtime_get_peer(
    size_t index, pwos_host_rpc_peer_snapshot_t *out_peer);

/** 获取拓扑表中第 index 个节点 */
int pwos_host_rpc_runtime_get_topology_node(
    size_t index, pwos_host_rpc_topology_node_t *out_node);

/* ---- 虚拟文件读写 (自动路由: 本地 coordinator / 远端 peer) ---- */

int pwos_host_rpc_runtime_read_path(
    const char *path, uint8_t *data,
    uint16_t *in_out_len, uint32_t deadline_ms);

int pwos_host_rpc_runtime_write_path(
    const char *path, const uint8_t *data,
    uint16_t data_len, uint16_t *out_written,
    uint32_t deadline_ms);

/* ---- 分布式推理 (hostname=NULL 为本地) ---- */

int pwos_host_rpc_runtime_llm_submit(
    const char *hostname, const char *prompt,
    uint32_t deadline_ms);

int pwos_host_rpc_runtime_llm_result(
    const char *hostname, uint8_t *out,
    uint16_t *in_out_len, uint32_t deadline_ms);

int pwos_host_rpc_runtime_llm_status(
    const char *hostname, uint8_t *out,
    uint16_t *in_out_len, uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_RPC_RUNTIME_H */
