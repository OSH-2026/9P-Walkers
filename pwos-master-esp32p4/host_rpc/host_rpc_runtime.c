/* ========================================================================
 * Host RPC 运行时 —— 实现
 *
 * 多主机协同核心，包含三大任务:
 *   1. server_task     — TCP Server (端口 9909), 处理远端 RPC 请求
 *   2. discovery_task  — mDNS 发现 + advertise + 拓扑同步 + peer 过期
 *   3. 分布式推理接口  — llm_submit / result / status (本地/远程路由)
 *
 * 数据结构:
 *   g_host_rpc.election       — Leader/Follower 选举状态机
 *   g_host_rpc.topology       — 全局 MCU 命名空间 (mcu1, mcu2, ...)
 *   g_host_rpc.peers[]        — 对等主机表 (mDNS 发现 + advertise 填充)
 *   g_host_rpc.local_advertise — 本机公告信息
 *
 * 线程安全: runtime_lock() / runtime_unlock() 保护全局状态
 * ======================================================================== */

#include "host_rpc_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "host_rpc_peer_client.h"
#include "host_rpc_service.h"
#include "lan_runtime.h"
#include "mini9p_protocol.h"
#include "pwos_coordinator_runtime.h"
#include "session_manager.h"
#include "dist_inference_service.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

/* ========================================================================
 * 第一节: 配置常量
 * ======================================================================== */

#ifndef PWOS_HOST_RPC_LOCAL_PRIORITY
#define PWOS_HOST_RPC_LOCAL_PRIORITY     200u    /* 本机选举优先级           */
#endif
#define PWOS_HOST_RPC_SERVER_STACK       16384u  /* server 任务栈            */
#define PWOS_HOST_RPC_DISCOVERY_STACK    24576u  /* discovery 任务栈 (较大)   */
#define PWOS_HOST_RPC_TASK_PRIORITY      4u      /* 任务优先级               */
#define PWOS_HOST_RPC_DISCOVERY_INTERVAL_MS  5000u   /* 发现间隔               */
#define PWOS_HOST_RPC_DISCOVERY_QUERY_MS     1200u   /* mDNS 查询超时           */
#define PWOS_HOST_RPC_DEFAULT_DEADLINE_MS    1500u   /* 默认 TCP 超时            */

/* ========================================================================
 * 第二节: 内部类型
 * ======================================================================== */

typedef struct {
    uint8_t                       used;
    pwos_host_rpc_peer_snapshot_t snapshot;
} runtime_peer_t;

typedef struct {
    char     ip[16];
    uint16_t port;
} exchange_target_t;

typedef struct {
    pwos_host_rpc_service_t     service;          /* RPC 服务端              */
    pwos_host_election_t        election;         /* 选举状态机              */
    runtime_peer_t              peers[PWOS_HOST_RPC_MAX_PEERS];  /* peer 表   */
    pwos_host_rpc_topology_t    topology;         /* 全局拓扑                */
    pwos_host_rpc_advertise_t   local_advertise;  /* 本机公告                */
    pwos_host_rpc_runtime_status_t status;        /* 运行时状态               */
    char current_remote_ip[16];                   /* 当前 TCP 客户端 IP       */
#ifdef ESP_PLATFORM
    SemaphoreHandle_t mutex;
    TaskHandle_t      server_task;
    TaskHandle_t      discovery_task;
#endif
} host_rpc_runtime_t;

/* ========================================================================
 * 第三节: 全局状态 & 同步原语
 * ======================================================================== */

static host_rpc_runtime_t g_host_rpc;
#ifdef ESP_PLATFORM
static const char *TAG = "pwos_host_rpc";

static void runtime_lock(void)   { xSemaphoreTake(g_host_rpc.mutex, portMAX_DELAY); }
static void runtime_unlock(void) { xSemaphoreGive(g_host_rpc.mutex); }
static uint32_t now_ms(void)     { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
#endif

/* ========================================================================
 * 第四节: NVS Epoch 管理 (持久化选举纪元)
 * ======================================================================== */
#ifdef ESP_PLATFORM

static uint32_t load_next_epoch(void)
{
    static const char *ns  = "pwos_host";
    static const char *key = "epoch";
    nvs_handle_t handle;
    uint32_t epoch = 0u;
    esp_err_t err;

    err = nvs_flash_init();
    if (err == ESP_OK) err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u32(handle, key, &epoch);
        if (err == ESP_ERR_NVS_NOT_FOUND) { epoch = 0u; err = ESP_OK; }
        if (err == ESP_OK) {
            epoch++;
            if (epoch == 0u) epoch = 1u;
            err = nvs_set_u32(handle, key, epoch);
        }
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) return epoch;

    /* NVS 故障时用随机值降级运行 */
    epoch = esp_random();
    if (epoch == 0u) epoch = 1u;
    ESP_LOGW(TAG, "NVS epoch 不可用: %s, fallback=%lu",
             esp_err_to_name(err), (unsigned long)epoch);
    return epoch;
}
#endif

/* ========================================================================
 * 第五节: Peer & 拓扑管理 (内部辅助函数)
 * ======================================================================== */
#ifdef ESP_PLATFORM

static int uid_equal(const uint32_t a[3], const uint32_t b[3])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static void refresh_role_locked(void)
{
    g_host_rpc.status.local_role = g_host_rpc.election.local_role;
    memcpy(g_host_rpc.status.leader_uid, g_host_rpc.election.leader.uid,
           sizeof(g_host_rpc.status.leader_uid));
    g_host_rpc.local_advertise.role = g_host_rpc.election.local_role;
}

/** 按 UID 查找 peer (调用者需持锁) */
static runtime_peer_t *find_peer_uid_locked(const uint32_t uid[3])
{
    for (size_t i = 0; i < PWOS_HOST_RPC_MAX_PEERS; i++) {
        if (g_host_rpc.peers[i].used &&
            uid_equal(g_host_rpc.peers[i].snapshot.uid, uid))
            return &g_host_rpc.peers[i];
    }
    return NULL;
}

/** 按 IP 查找 peer */
static runtime_peer_t *find_peer_ip_locked(const char *ip)
{
    for (size_t i = 0; i < PWOS_HOST_RPC_MAX_PEERS; i++) {
        if (g_host_rpc.peers[i].used &&
            strcmp(g_host_rpc.peers[i].snapshot.ip, ip) == 0)
            return &g_host_rpc.peers[i];
    }
    return NULL;
}

/** 按 hostname 查找 peer */
static runtime_peer_t *find_peer_hostname_locked(const char *hostname)
{
    for (size_t i = 0; i < PWOS_HOST_RPC_MAX_PEERS; i++) {
        if (g_host_rpc.peers[i].used &&
            strcmp(g_host_rpc.peers[i].snapshot.hostname, hostname) == 0)
            return &g_host_rpc.peers[i];
    }
    return NULL;
}

/** 插入或更新 peer 记录 */
static int upsert_peer_locked(const pwos_host_rpc_advertise_t *advertise,
                              const char *ip)
{
    /* 忽略自己的公告 */
    if (uid_equal(advertise->uid, g_host_rpc.local_advertise.uid)) return -1;

    runtime_peer_t *peer = find_peer_uid_locked(advertise->uid);
    if (!peer) {
        for (size_t i = 0; i < PWOS_HOST_RPC_MAX_PEERS; i++) {
            if (!g_host_rpc.peers[i].used) {
                peer = &g_host_rpc.peers[i];
                memset(peer, 0, sizeof(*peer));
                peer->used = 1u;
                peer->snapshot.used = 1u;
                g_host_rpc.status.peer_count++;
                break;
            }
        }
    }
    if (!peer) return -1;

    memcpy(peer->snapshot.uid, advertise->uid, sizeof(peer->snapshot.uid));
    peer->snapshot.epoch       = advertise->epoch;
    peer->snapshot.priority    = advertise->priority;
    peer->snapshot.role        = advertise->role;
    peer->snapshot.port        = advertise->rpc_port;
    peer->snapshot.last_seen_ms = now_ms();
    snprintf(peer->snapshot.hostname, sizeof(peer->snapshot.hostname),
             "%s", advertise->hostname);
    if (ip && ip[0]) snprintf(peer->snapshot.ip, sizeof(peer->snapshot.ip), "%s", ip);

    if (pwos_host_election_update_peer(&g_host_rpc.election,
                                       advertise->uid, advertise->epoch,
                                       advertise->priority,
                                       peer->snapshot.last_seen_ms) != 0) return -1;
    refresh_role_locked();
    return 0;
}

/** 判断全局名称是否已被其他节点占用 */
static int topology_name_used_locked(const char *name, const uint32_t node_uid[3])
{
    for (size_t i = 0; i < g_host_rpc.topology.node_count; i++) {
        const pwos_host_rpc_topology_node_t *n = &g_host_rpc.topology.nodes[i];
        if (strcmp(n->global_target, name) == 0 && !uid_equal(n->node_uid, node_uid))
            return 1;
    }
    return 0;
}

/** 为新节点分配 mcuN 格式的全局名称 */
static void allocate_global_name_locked(char out[PWOS_HOST_RPC_TARGET_CAP],
                                        const uint32_t node_uid[3])
{
    for (unsigned num = 1; num < 10000; num++) {
        char candidate[PWOS_HOST_RPC_TARGET_CAP];
        snprintf(candidate, sizeof(candidate), "mcu%u", num);
        if (!topology_name_used_locked(candidate, node_uid)) {
            snprintf(out, PWOS_HOST_RPC_TARGET_CAP, "%s", candidate);
            return;
        }
    }
    out[0] = '\0';
}

/** 判断快照中是否包含指定节点 */
static int topology_snapshot_has_node(const pwos_host_rpc_topology_t *snap,
                                      const uint32_t node_uid[3])
{
    if (!snap) return 0;
    for (size_t i = 0; i < snap->node_count; i++)
        if (uid_equal(snap->nodes[i].node_uid, node_uid)) return 1;
    return 0;
}

/** 从本地拓扑中移除某 owner 拥有的、且不在快照中的节点 (GC) */
static void prune_owner_locked(const uint32_t owner_uid[3],
                               const pwos_host_rpc_topology_t *snapshot)
{
    size_t i = 0;
    while (i < g_host_rpc.topology.node_count) {
        pwos_host_rpc_topology_node_t *node = &g_host_rpc.topology.nodes[i];
        if (!uid_equal(node->owner_uid, owner_uid) ||
            topology_snapshot_has_node(snapshot, node->node_uid)) { i++; continue; }

        g_host_rpc.topology.node_count--;
        if (i < g_host_rpc.topology.node_count)
            memmove(&g_host_rpc.topology.nodes[i],
                    &g_host_rpc.topology.nodes[i + 1],
                    (g_host_rpc.topology.node_count - i) * sizeof(g_host_rpc.topology.nodes[0]));
        memset(&g_host_rpc.topology.nodes[g_host_rpc.topology.node_count], 0,
               sizeof(g_host_rpc.topology.nodes[0]));
    }
    g_host_rpc.status.topology_nodes = g_host_rpc.topology.node_count;
}

/** 从 incoming 拓扑中提取某 owner 拥有的节点子集 */
static void filter_owner_topology(const pwos_host_rpc_topology_t *incoming,
                                  const uint32_t owner_uid[3],
                                  pwos_host_rpc_topology_t *out)
{
    memset(out, 0, sizeof(*out));
    out->generation = incoming->generation;
    for (size_t i = 0; i < incoming->node_count; i++) {
        if (!uid_equal(incoming->nodes[i].owner_uid, owner_uid)) continue;
        out->nodes[out->node_count++] = incoming->nodes[i];
    }
}

/**
 * 合并拓扑: 新节点追加，已知节点更新 (保留已有全局名称)。
 * @param trust_global_names  1=用 incoming 的全局名称 (follower 信任 leader)
 */
static void merge_topology_locked(const pwos_host_rpc_topology_t *incoming,
                                  uint8_t trust_global_names)
{
    if (trust_global_names) {
        g_host_rpc.topology = *incoming;
        g_host_rpc.status.topology_nodes = g_host_rpc.topology.node_count;
        return;
    }

    for (size_t i = 0; i < incoming->node_count; i++) {
        const pwos_host_rpc_topology_node_t *src = &incoming->nodes[i];
        pwos_host_rpc_topology_node_t *dst = NULL;

        for (size_t j = 0; j < g_host_rpc.topology.node_count; j++) {
            if (uid_equal(g_host_rpc.topology.nodes[j].node_uid, src->node_uid)) {
                dst = &g_host_rpc.topology.nodes[j]; break;
            }
        }
        if (!dst) {
            if (g_host_rpc.topology.node_count >= PWOS_HOST_RPC_TOPOLOGY_MAX_NODES) continue;
            dst = &g_host_rpc.topology.nodes[g_host_rpc.topology.node_count++];
            memset(dst, 0, sizeof(*dst));
        }

        memcpy(dst->owner_uid, src->owner_uid, sizeof(dst->owner_uid));
        memcpy(dst->node_uid,  src->node_uid,  sizeof(dst->node_uid));
        dst->boot_id = src->boot_id;
        snprintf(dst->owner_target, sizeof(dst->owner_target), "%s", src->owner_target);

        if (!dst->global_target[0]) {
            if (src->global_target[0] &&
                !topology_name_used_locked(src->global_target, src->node_uid)) {
                snprintf(dst->global_target, sizeof(dst->global_target),
                         "%s", src->global_target);
            } else {
                allocate_global_name_locked(dst->global_target, src->node_uid);
            }
        }
    }
    g_host_rpc.topology.generation++;
    g_host_rpc.status.topology_nodes = g_host_rpc.topology.node_count;
}

/* ========================================================================
 * 第六节: 本地拓扑构建 & 刷新
 * ======================================================================== */

static int build_local_topology(pwos_host_rpc_topology_t *out)
{
    memset(out, 0, sizeof(*out));
    runtime_lock();
    out->generation = g_host_rpc.topology.generation;
    runtime_unlock();

    for (size_t i = 0; i < PWOS_CLUSTER_VFS_MAX_ROUTES &&
         out->node_count < PWOS_HOST_RPC_TOPOLOGY_MAX_NODES; i++) {
        pwos_cluster_vfs_route_t route;
        if (pwos_coordinator_runtime_get_route(i, &route) != 0 ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE) continue;

        pwos_host_rpc_topology_node_t *node = &out->nodes[out->node_count++];
        snprintf(node->global_target, sizeof(node->global_target), "%s", route.target);
        snprintf(node->owner_target,  sizeof(node->owner_target),  "%s", route.target);
        runtime_lock();
        memcpy(node->owner_uid, g_host_rpc.local_advertise.uid, sizeof(node->owner_uid));
        runtime_unlock();
        memcpy(node->node_uid, route.uid, sizeof(node->node_uid));
        node->boot_id = route.boot_id;
    }
    return 0;
}

static void refresh_local_topology(void)
{
    pwos_host_rpc_topology_t local;
    build_local_topology(&local);
    runtime_lock();
    prune_owner_locked(g_host_rpc.local_advertise.uid, &local);
    merge_topology_locked(&local, 0u);
    runtime_unlock();
}

/* ========================================================================
 * 第七节: RPC 服务回调 (供 host_rpc_service 调用)
 * ======================================================================== */

static int service_read_node(void *ctx, const char *target, const char *path,
                             uint8_t *data, uint16_t *in_out_len, uint32_t deadline_ms)
{
    (void)ctx;
    char full[PWOS_HOST_RPC_TARGET_CAP + PWOS_HOST_RPC_PATH_CAP + 2u];

    if (strcmp(target, "llm") == 0) {
        snprintf(full, sizeof(full), "/llm%s", path);
        return pwos_dist_inference_service_read(full, data, in_out_len, deadline_ms);
    }
    snprintf(full, sizeof(full), "/%s%s", target, path);
    return pwos_coordinator_runtime_read_path(full, data, in_out_len, deadline_ms);
}

static int service_write_node(void *ctx, const char *target, const char *path,
                              const uint8_t *data, uint16_t data_len,
                              uint16_t *out_written, uint32_t deadline_ms)
{
    (void)ctx;
    char full[PWOS_HOST_RPC_TARGET_CAP + PWOS_HOST_RPC_PATH_CAP + 2u];

    if (strcmp(target, "llm") == 0) {
        snprintf(full, sizeof(full), "/llm%s", path);
        return pwos_dist_inference_service_write(full, data, data_len, out_written, deadline_ms);
    }
    snprintf(full, sizeof(full), "/%s%s", target, path);
    return pwos_coordinator_runtime_write_path(full, data, data_len, out_written, deadline_ms);
}

static int service_advertise(void *ctx, const pwos_host_rpc_advertise_t *advertise)
{
    (void)ctx;
    runtime_lock();
    int rc = upsert_peer_locked(advertise, g_host_rpc.current_remote_ip);
    runtime_unlock();
    return rc != 0 ? -(int)M9P_ERR_EBUSY : 0;
}

static int service_local_advertise(void *ctx, pwos_host_rpc_advertise_t *out)
{
    (void)ctx;
    runtime_lock();
    *out = g_host_rpc.local_advertise;
    runtime_unlock();
    return 0;
}

static int service_whoowns(void *ctx, const char *target,
                           pwos_host_rpc_advertise_t *out_owner)
{
    (void)ctx;
    runtime_lock();
    for (size_t i = 0; i < g_host_rpc.topology.node_count; i++) {
        const pwos_host_rpc_topology_node_t *node = &g_host_rpc.topology.nodes[i];
        if (strcmp(node->global_target, target) != 0) continue;

        if (uid_equal(node->owner_uid, g_host_rpc.local_advertise.uid)) {
            *out_owner = g_host_rpc.local_advertise;
            runtime_unlock(); return 0;
        }
        runtime_peer_t *peer = find_peer_uid_locked(node->owner_uid);
        if (peer) {
            memset(out_owner, 0, sizeof(*out_owner));
            memcpy(out_owner->uid, peer->snapshot.uid, sizeof(out_owner->uid));
            out_owner->epoch    = peer->snapshot.epoch;
            out_owner->priority = peer->snapshot.priority;
            out_owner->role     = peer->snapshot.role;
            out_owner->rpc_port = peer->snapshot.port;
            snprintf(out_owner->hostname, sizeof(out_owner->hostname),
                     "%s", peer->snapshot.hostname);
            runtime_unlock(); return 0;
        }
    }
    runtime_unlock();
    return -(int)M9P_ERR_ENOENT;
}

static int service_topology_sync(void *ctx,
                                 const pwos_host_rpc_topology_t *incoming,
                                 pwos_host_rpc_topology_t *out_current)
{
    (void)ctx;
    pwos_host_rpc_topology_t local, peer_snap;
    build_local_topology(&local);

    runtime_lock();
    runtime_peer_t *src = find_peer_ip_locked(g_host_rpc.current_remote_ip);
    uint8_t trust = (g_host_rpc.election.local_role == PWOS_HOST_ROLE_FOLLOWER &&
                     src && uid_equal(src->snapshot.uid, g_host_rpc.election.leader.uid));

    prune_owner_locked(g_host_rpc.local_advertise.uid, &local);
    merge_topology_locked(&local, 0u);

    if (trust) {
        merge_topology_locked(incoming, 1u);
    } else if (src) {
        filter_owner_topology(incoming, src->snapshot.uid, &peer_snap);
        prune_owner_locked(src->snapshot.uid, &peer_snap);
        merge_topology_locked(&peer_snap, 0u);
    }

    if (g_host_rpc.election.local_role == PWOS_HOST_ROLE_LEADER)
        *out_current = g_host_rpc.topology;
    else {
        *out_current = local;
        out_current->generation = g_host_rpc.topology.generation;
    }
    runtime_unlock();
    return 0;
}

/* ========================================================================
 * 第八节: TCP 网络层
 * ======================================================================== */

static int send_all(int fd, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int rc = send(fd, data + sent, len - sent, 0);
        if (rc <= 0) return -1;
        sent += (size_t)rc;
    }
    return 0;
}

static int recv_all(int fd, uint8_t *data, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int rc = recv(fd, data + received, len - received, 0);
        if (rc <= 0) return -1;
        received += (size_t)rc;
    }
    return 0;
}

static void set_socket_timeout(int fd, uint32_t deadline_ms)
{
    if (deadline_ms == 0u) deadline_ms = PWOS_HOST_RPC_DEFAULT_DEADLINE_MS;
    struct timeval tv = {
        .tv_sec  = (time_t)(deadline_ms / 1000u),
        .tv_usec = (suseconds_t)((deadline_ms % 1000u) * 1000u),
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/** TCP 帧交换: connect → send → recv prefix → recv body → close */
static int tcp_exchange(void *ctx, const uint8_t *request, size_t request_len,
                        uint8_t *response, size_t response_cap,
                        size_t *out_response_len, uint32_t deadline_ms)
{
    const exchange_target_t *target = (const exchange_target_t *)ctx;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(target->port) };
    if (inet_pton(AF_INET, target->ip, &addr.sin_addr) != 1) return -(int)M9P_ERR_EINVAL;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -(int)M9P_ERR_EIO;

    set_socket_timeout(fd, deadline_ms);
    int rc = -(int)M9P_ERR_EIO;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        send_all(fd, request, request_len) != 0 ||
        response_cap < PWOS_HOST_RPC_PREFIX_LEN ||
        recv_all(fd, response, PWOS_HOST_RPC_PREFIX_LEN) != 0) {
        if (errno == EAGAIN || errno == ETIMEDOUT) rc = PWOS_SESSION_ERR_DEADLINE;
        goto done;
    }

    uint32_t body_len = pwos_host_rpc_body_len(response);
    if (body_len == 0u || body_len > response_cap - PWOS_HOST_RPC_PREFIX_LEN ||
        body_len > PWOS_HOST_RPC_MAX_FRAME_LEN - PWOS_HOST_RPC_PREFIX_LEN ||
        recv_all(fd, response + PWOS_HOST_RPC_PREFIX_LEN, body_len) != 0) goto done;

    *out_response_len = PWOS_HOST_RPC_PREFIX_LEN + body_len;
    rc = 0;

done:
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return rc;
}

/* ========================================================================
 * 第九节: 远端调用辅助
 * ======================================================================== */

/** 向指定 IP:port 发起 RPC call */
static int call_address(const char *ip, uint16_t port,
                        const char *service, const char *method,
                        const uint8_t *payload, uint16_t payload_len,
                        uint32_t deadline_ms,
                        uint8_t *response, uint16_t *in_out_response_len,
                        uint16_t *out_status)
{
    exchange_target_t target;
    snprintf(target.ip, sizeof(target.ip), "%s", ip);
    target.port = port;

    pwos_host_rpc_peer_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.io_ctx   = &target;
    config.exchange = tcp_exchange;

    pwos_host_rpc_peer_client_t client;
    if (pwos_host_rpc_peer_client_init(&client, &config) != 0)
        return -(int)M9P_ERR_EINVAL;

    int rc = pwos_host_rpc_peer_client_call(&client, service, method,
                                            payload, payload_len, deadline_ms,
                                            response, in_out_response_len, out_status);
    runtime_lock();
    g_host_rpc.status.client_calls++;
    if (rc != 0 || *out_status != PWOS_HOST_RPC_STATUS_OK) {
        g_host_rpc.status.client_errors++;
        g_host_rpc.status.last_error = rc ? rc
            : -(PWOS_HOST_RPC_REMOTE_ERROR_BASE + (int)*out_status);
    } else {
        g_host_rpc.status.last_error = 0;
    }
    runtime_unlock();
    return rc;
}

/** 向远端发送公告并获取对方公告 */
static int advertise_to(const char *ip, uint16_t port,
                        pwos_host_rpc_advertise_t *out_remote)
{
    uint8_t args[256], response[256];
    uint16_t args_len = 0u, response_len = sizeof(response);
    uint16_t status;
    pwos_host_rpc_advertise_t local;

    runtime_lock(); local = g_host_rpc.local_advertise; runtime_unlock();

    if (pwos_host_rpc_encode_advertise(&local, args, sizeof(args), &args_len) != 0)
        return -(int)M9P_ERR_EINVAL;

    int rc = call_address(ip, port, "host", "advertise",
                          args, args_len, PWOS_HOST_RPC_DEFAULT_DEADLINE_MS,
                          response, &response_len, &status);
    if (rc != 0) return rc;
    if (status != PWOS_HOST_RPC_STATUS_OK ||
        pwos_host_rpc_decode_advertise(response, response_len, out_remote) != 0)
        return -(int)M9P_ERR_EIO;
    return 0;
}

/** 与远端同步拓扑 */
static int topology_sync_to(const char *ip, uint16_t port, const uint32_t peer_uid[3])
{
    pwos_host_rpc_topology_t outgoing, incoming, peer_snap;
    uint8_t args[PWOS_HOST_RPC_MAX_PAYLOAD_LEN], response[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint16_t args_len = 0u, response_len = sizeof(response);
    uint16_t status;

    refresh_local_topology();
    runtime_lock();
    outgoing = (g_host_rpc.election.local_role == PWOS_HOST_ROLE_LEADER)
        ? g_host_rpc.topology : (pwos_host_rpc_topology_t){0};
    runtime_unlock();
    if (outgoing.node_count == 0u) build_local_topology(&outgoing);

    if (pwos_host_rpc_encode_topology(&outgoing, args, sizeof(args), &args_len) != 0)
        return -(int)M9P_ERR_EMSIZE;

    int rc = call_address(ip, port, "topology", "sync",
                          args, args_len, PWOS_HOST_RPC_DEFAULT_DEADLINE_MS,
                          response, &response_len, &status);
    if (rc != 0) return rc;
    if (status != PWOS_HOST_RPC_STATUS_OK ||
        pwos_host_rpc_decode_topology(response, response_len, &incoming) != 0)
        return -(int)M9P_ERR_EIO;

    runtime_lock();
    if (uid_equal(peer_uid, g_host_rpc.election.leader.uid))
        merge_topology_locked(&incoming, 1u);
    else {
        filter_owner_topology(&incoming, peer_uid, &peer_snap);
        prune_owner_locked(peer_uid, &peer_snap);
        merge_topology_locked(&peer_snap, 0u);
    }
    runtime_unlock();
    return 0;
}

/* ========================================================================
 * 第十节: mDNS 发现 & Peer 过期
 * ======================================================================== */

static void process_mdns_result(mdns_result_t *result)
{
    if (!result || !result->hostname || !result->port ||
        strcmp(result->hostname, g_host_rpc.status.hostname) == 0) return;

    for (mdns_ip_addr_t *addr = result->addr; addr; addr = addr->next) {
        if (addr->addr.type == ESP_IPADDR_TYPE_V6) continue;

        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&addr->addr.u_addr.ip4));

        pwos_host_rpc_advertise_t remote;
        memset(&remote, 0, sizeof(remote));
        int rc = advertise_to(ip, result->port, &remote);

        runtime_lock();
        g_host_rpc.status.discovery_results++;
        if (rc == 0 && upsert_peer_locked(&remote, ip) == 0)
            g_host_rpc.status.advertise_ok++;
        else {
            g_host_rpc.status.advertise_fail++;
            g_host_rpc.status.last_error = rc;
        }
        runtime_unlock();

        if (rc == 0) {
            rc = topology_sync_to(ip, remote.rpc_port, remote.uid);
            runtime_lock();
            if (rc == 0) g_host_rpc.status.topology_sync_ok++;
            else { g_host_rpc.status.topology_sync_fail++;
                   g_host_rpc.status.last_error = rc; }
            runtime_unlock();
        }
        break;  /* 每个 peer 只处理一个 IPv4 地址 */
    }
}

static void expire_peers(void)
{
    uint32_t now = now_ms();
    runtime_lock();
    pwos_host_election_expire(&g_host_rpc.election, now);
    for (size_t i = 0; i < PWOS_HOST_RPC_MAX_PEERS; i++) {
        if (g_host_rpc.peers[i].used &&
            (uint32_t)(now - g_host_rpc.peers[i].snapshot.last_seen_ms) >=
                g_host_rpc.election.timeout_ms) {
            prune_owner_locked(g_host_rpc.peers[i].snapshot.uid, NULL);
            memset(&g_host_rpc.peers[i], 0, sizeof(g_host_rpc.peers[i]));
            if (g_host_rpc.status.peer_count > 0u) g_host_rpc.status.peer_count--;
        }
    }
    refresh_role_locked();
    runtime_unlock();
}

/* ========================================================================
 * 第十一节: Server 任务 (TCP accept 循环)
 * ======================================================================== */

static void handle_client_socket(int client_fd, const char *remote_ip)
{
    uint8_t request[PWOS_HOST_RPC_MAX_FRAME_LEN];
    uint8_t response[PWOS_HOST_RPC_MAX_FRAME_LEN];
    size_t response_len = 0u;

    set_socket_timeout(client_fd, 2000u);

    if (recv_all(client_fd, request, PWOS_HOST_RPC_PREFIX_LEN) != 0) goto fail;
    uint32_t body_len = pwos_host_rpc_body_len(request);
    if (body_len == 0u || body_len > sizeof(request) - PWOS_HOST_RPC_PREFIX_LEN ||
        recv_all(client_fd, request + PWOS_HOST_RPC_PREFIX_LEN, body_len) != 0) goto fail;

    snprintf(g_host_rpc.current_remote_ip, sizeof(g_host_rpc.current_remote_ip), "%s", remote_ip);

    if (pwos_host_rpc_service_handle(&g_host_rpc.service,
                                     request, PWOS_HOST_RPC_PREFIX_LEN + body_len,
                                     response, sizeof(response), &response_len) != 0 ||
        send_all(client_fd, response, response_len) != 0) goto fail;

    runtime_lock(); g_host_rpc.status.server_requests++; runtime_unlock();
    return;

fail:
    runtime_lock(); g_host_rpc.status.server_errors++; runtime_unlock();
}

static void server_task(void *arg)
{
    (void)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) goto fatal;

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons(PWOS_HOST_RPC_PORT),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 4) != 0) {
        close(listen_fd);
        goto fatal;
    }

    runtime_lock(); g_host_rpc.status.server_started = 1u; runtime_unlock();
    ESP_LOGI(TAG, "server listening tcp/%u", PWOS_HOST_RPC_PORT);

    for (;;) {
        struct sockaddr_in remote;
        socklen_t remote_len = sizeof(remote);
        int client_fd = accept(listen_fd, (struct sockaddr *)&remote, &remote_len);
        if (client_fd < 0) continue;

        char remote_ip[16];
        inet_ntop(AF_INET, &remote.sin_addr, remote_ip, sizeof(remote_ip));

        runtime_lock(); g_host_rpc.status.accepted++; runtime_unlock();
        handle_client_socket(client_fd, remote_ip);
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
    }

fatal:
    runtime_lock();
    g_host_rpc.status.server_errors++;
    g_host_rpc.status.last_error = -(int)M9P_ERR_EIO;
    runtime_unlock();
    vTaskDelete(NULL);
}

/* ========================================================================
 * 第十二节: Discovery 任务 (mDNS + 周期维护)
 * ======================================================================== */

static void discovery_task(void *arg)
{
    (void)arg;
    runtime_lock(); g_host_rpc.status.discovery_started = 1u; runtime_unlock();

    for (;;) {
        pwos_lan_runtime_status_t lan;
        pwos_lan_runtime_get_status(&lan);

        if (lan.has_ip && lan.mdns_ready) {
            runtime_lock(); g_host_rpc.status.discovery_queries++; runtime_unlock();

            mdns_result_t *results = NULL;
            if (mdns_query_ptr("_pwos", "_tcp", PWOS_HOST_RPC_DISCOVERY_QUERY_MS,
                               PWOS_HOST_RPC_MAX_PEERS + 1u, &results) == ESP_OK) {
                for (mdns_result_t *r = results; r; r = r->next)
                    process_mdns_result(r);
                mdns_query_results_free(results);
            }
        }

        expire_peers();
        vTaskDelay(pdMS_TO_TICKS(PWOS_HOST_RPC_DISCOVERY_INTERVAL_MS));
    }
}

/* ========================================================================
 * 第十三节: 运行时启动
 * ======================================================================== */

int pwos_host_rpc_runtime_start(void)
{
    if (g_host_rpc.status.initialized) return 0;

    pwos_lan_runtime_status_t lan;
    pwos_lan_runtime_get_status(&lan);
    if (!lan.initialized || !lan.hostname[0]) return -(int)M9P_ERR_EIO;

    memset(&g_host_rpc, 0, sizeof(g_host_rpc));
    g_host_rpc.mutex = xSemaphoreCreateMutex();
    if (!g_host_rpc.mutex) return -(int)M9P_ERR_EBUSY;

    /* 构造 UID (基于 MAC 地址) */
    uint32_t uid[3];
    uid[0] = ((uint32_t)lan.mac[0] << 24) | ((uint32_t)lan.mac[1] << 16) |
             ((uint32_t)lan.mac[2] << 8) | lan.mac[3];
    uid[1] = ((uint32_t)lan.mac[4] << 24) | ((uint32_t)lan.mac[5] << 16) | 0x5057u;
    uid[2] = 0x484F5354u; /* "HOST" */
    uint32_t epoch = load_next_epoch();

    /* 初始化选举 */
    if (pwos_host_election_init(&g_host_rpc.election, uid, epoch,
                                PWOS_HOST_RPC_LOCAL_PRIORITY, 0u) != 0)
        return -(int)M9P_ERR_EINVAL;

    /* 构建本地公告 */
    memcpy(g_host_rpc.local_advertise.uid, uid, sizeof(uid));
    g_host_rpc.local_advertise.epoch    = g_host_rpc.election.local.epoch;
    g_host_rpc.local_advertise.priority = PWOS_HOST_RPC_LOCAL_PRIORITY;
    g_host_rpc.local_advertise.role     = PWOS_HOST_ROLE_LEADER;
    g_host_rpc.local_advertise.rpc_port = PWOS_HOST_RPC_PORT;
    snprintf(g_host_rpc.local_advertise.hostname,
             sizeof(g_host_rpc.local_advertise.hostname), "%s", lan.hostname);

    /* 初始化 RPC 服务 */
    pwos_host_rpc_service_config_t svc_cfg;
    memset(&svc_cfg, 0, sizeof(svc_cfg));
    svc_cfg.read_node        = service_read_node;
    svc_cfg.write_node       = service_write_node;
    svc_cfg.advertise        = service_advertise;
    svc_cfg.local_advertise  = service_local_advertise;
    svc_cfg.whoowns          = service_whoowns;
    svc_cfg.topology_sync    = service_topology_sync;
    if (pwos_host_rpc_service_init(&g_host_rpc.service, &svc_cfg) != 0)
        return -(int)M9P_ERR_EINVAL;

    /* 初始化状态快照 */
    g_host_rpc.status.initialized    = 1u;
    g_host_rpc.status.local_role     = PWOS_HOST_ROLE_LEADER;
    memcpy(g_host_rpc.status.local_uid, uid, sizeof(uid));
    g_host_rpc.status.local_epoch    = g_host_rpc.local_advertise.epoch;
    g_host_rpc.status.local_priority = PWOS_HOST_RPC_LOCAL_PRIORITY;
    memcpy(g_host_rpc.status.leader_uid, uid, sizeof(uid));
    snprintf(g_host_rpc.status.hostname, sizeof(g_host_rpc.status.hostname),
             "%s", lan.hostname);

    refresh_local_topology();

    /* 发布 mDNS 服务 */
    pwos_lan_runtime_publish_host_rpc(uid, g_host_rpc.local_advertise.epoch,
                                      PWOS_HOST_RPC_LOCAL_PRIORITY, PWOS_HOST_RPC_PORT);

    /* 创建任务 */
    BaseType_t ok;
    ok = xTaskCreate(server_task, "host_rpc_srv", PWOS_HOST_RPC_SERVER_STACK,
                     NULL, PWOS_HOST_RPC_TASK_PRIORITY, &g_host_rpc.server_task);
    if (ok != pdPASS) return -(int)M9P_ERR_EBUSY;

    ok = xTaskCreate(discovery_task, "host_discovery", PWOS_HOST_RPC_DISCOVERY_STACK,
                     NULL, PWOS_HOST_RPC_TASK_PRIORITY, &g_host_rpc.discovery_task);
    if (ok != pdPASS) {
        vTaskDelete(g_host_rpc.server_task);
        g_host_rpc.server_task = NULL;
        return -(int)M9P_ERR_EBUSY;
    }

    ESP_LOGI(TAG, "started host=%s epoch=%lu uid=%08lx-%08lx-%08lx",
             g_host_rpc.status.hostname, (unsigned long)g_host_rpc.status.local_epoch,
             (unsigned long)uid[0], (unsigned long)uid[1], (unsigned long)uid[2]);
    return 0;
}

#else /* !ESP_PLATFORM */

int pwos_host_rpc_runtime_start(void) { return -1; }

#endif

/* ========================================================================
 * 第十四节: 公开 API (状态查询 & 路径读写 & 分布式推理)
 * ======================================================================== */

void pwos_host_rpc_runtime_get_status(pwos_host_rpc_runtime_status_t *out)
{
    if (!out) return;
#ifdef ESP_PLATFORM
    if (g_host_rpc.mutex) runtime_lock();
#endif
    *out = g_host_rpc.status;
#ifdef ESP_PLATFORM
    if (g_host_rpc.mutex) runtime_unlock();
#endif
}

int pwos_host_rpc_runtime_get_peer(size_t index, pwos_host_rpc_peer_snapshot_t *out)
{
    if (!out || index >= PWOS_HOST_RPC_MAX_PEERS || !g_host_rpc.status.initialized)
        return -(int)M9P_ERR_EINVAL;
#ifdef ESP_PLATFORM
    runtime_lock();
#endif
    if (!g_host_rpc.peers[index].used) {
#ifdef ESP_PLATFORM
        runtime_unlock();
#endif
        return -(int)M9P_ERR_ENOENT;
    }
    *out = g_host_rpc.peers[index].snapshot;
#ifdef ESP_PLATFORM
    runtime_unlock();
#endif
    return 0;
}

int pwos_host_rpc_runtime_get_topology_node(size_t index, pwos_host_rpc_topology_node_t *out)
{
    if (!out || !g_host_rpc.status.initialized) return -(int)M9P_ERR_EINVAL;
#ifdef ESP_PLATFORM
    runtime_lock();
#endif
    if (index >= g_host_rpc.topology.node_count) {
#ifdef ESP_PLATFORM
        runtime_unlock();
#endif
        return -(int)M9P_ERR_ENOENT;
    }
    *out = g_host_rpc.topology.nodes[index];
#ifdef ESP_PLATFORM
    runtime_unlock();
#endif
    return 0;
}

#ifdef ESP_PLATFORM

/** 解析虚拟路径: /<global_target>/<rest> → 查拓扑表找到 owner host */
static int resolve_topology_path(const char *path, exchange_target_t *out_peer,
                                 char owner_target[PWOS_HOST_RPC_TARGET_CAP],
                                 char remote_path[PWOS_HOST_RPC_PATH_CAP],
                                 uint8_t *out_local_owner)
{
    if (!path || path[0] != '/' || !out_peer || !out_local_owner)
        return -(int)M9P_ERR_EINVAL;

    *out_local_owner = 0u;
    const char *sep = strchr(path + 1, '/');
    if (!sep) return -(int)M9P_ERR_EINVAL;

    size_t target_len = (size_t)(sep - (path + 1));
    if (target_len == 0u || target_len >= PWOS_HOST_RPC_TARGET_CAP ||
        strlen(sep) >= PWOS_HOST_RPC_PATH_CAP) return -(int)M9P_ERR_EINVAL;

    char global_target[PWOS_HOST_RPC_TARGET_CAP];
    memcpy(global_target, path + 1, target_len);
    global_target[target_len] = '\0';
    snprintf(remote_path, PWOS_HOST_RPC_PATH_CAP, "%s", sep);

    runtime_lock();
    for (size_t i = 0; i < g_host_rpc.topology.node_count; i++) {
        const pwos_host_rpc_topology_node_t *n = &g_host_rpc.topology.nodes[i];
        if (strcmp(n->global_target, global_target) != 0) continue;

        snprintf(owner_target, PWOS_HOST_RPC_TARGET_CAP, "%s", n->owner_target);
        if (uid_equal(n->owner_uid, g_host_rpc.local_advertise.uid)) {
            *out_local_owner = 1u; runtime_unlock(); return 0;
        }
        runtime_peer_t *peer = find_peer_uid_locked(n->owner_uid);
        if (!peer || !peer->snapshot.ip[0]) { runtime_unlock(); return PWOS_SESSION_ERR_NO_ROUTE; }
        snprintf(out_peer->ip, sizeof(out_peer->ip), "%s", peer->snapshot.ip);
        out_peer->port = peer->snapshot.port;
        runtime_unlock();
        return 0;
    }
    runtime_unlock();
    return PWOS_SESSION_ERR_NO_ROUTE;
}
#endif

int pwos_host_rpc_runtime_read_path(const char *path, uint8_t *data,
                                    uint16_t *in_out_len, uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    if (!g_host_rpc.status.initialized) return PWOS_SESSION_ERR_NO_ROUTE;

    if (strncmp(path, "/llm/", 5u) == 0)
        return pwos_dist_inference_service_read(path, data, in_out_len, deadline_ms);

    exchange_target_t peer;
    char owner_target[PWOS_HOST_RPC_TARGET_CAP], remote_path[PWOS_HOST_RPC_PATH_CAP];
    char local_path[PWOS_HOST_RPC_TARGET_CAP + PWOS_HOST_RPC_PATH_CAP + 2u];
    uint8_t local_owner;
    int rc = resolve_topology_path(path, &peer, owner_target, remote_path, &local_owner);
    if (rc != 0) return rc;

    if (local_owner) {
        snprintf(local_path, sizeof(local_path), "/%s%s", owner_target, remote_path);
        return pwos_coordinator_runtime_read_path(local_path, data, in_out_len, deadline_ms);
    }

    pwos_host_rpc_peer_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.io_ctx = &peer; cfg.exchange = tcp_exchange;
    pwos_host_rpc_peer_client_t client;
    if (pwos_host_rpc_peer_client_init(&client, &cfg) != 0) return -(int)M9P_ERR_EIO;

    rc = pwos_host_rpc_peer_client_read_node(&client, owner_target, remote_path,
                                             data, in_out_len, deadline_ms);
    runtime_lock();
    if (rc == 0) g_host_rpc.status.remote_reads++;
    else { g_host_rpc.status.client_errors++; g_host_rpc.status.last_error = rc; }
    runtime_unlock();
    return rc;
#else
    (void)path; (void)data; (void)in_out_len; (void)deadline_ms; return -1;
#endif
}

int pwos_host_rpc_runtime_write_path(const char *path, const uint8_t *data,
                                     uint16_t data_len, uint16_t *out_written,
                                     uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    if (!g_host_rpc.status.initialized) return PWOS_SESSION_ERR_NO_ROUTE;

    if (strncmp(path, "/llm/", 5u) == 0)
        return pwos_dist_inference_service_write(path, data, data_len, out_written, deadline_ms);

    exchange_target_t peer;
    char owner_target[PWOS_HOST_RPC_TARGET_CAP], remote_path[PWOS_HOST_RPC_PATH_CAP];
    char local_path[PWOS_HOST_RPC_TARGET_CAP + PWOS_HOST_RPC_PATH_CAP + 2u];
    uint8_t local_owner;
    int rc = resolve_topology_path(path, &peer, owner_target, remote_path, &local_owner);
    if (rc != 0) return rc;

    if (local_owner) {
        snprintf(local_path, sizeof(local_path), "/%s%s", owner_target, remote_path);
        return pwos_coordinator_runtime_write_path(local_path, data, data_len, out_written, deadline_ms);
    }

    pwos_host_rpc_peer_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.io_ctx = &peer; cfg.exchange = tcp_exchange;
    pwos_host_rpc_peer_client_t client;
    if (pwos_host_rpc_peer_client_init(&client, &cfg) != 0) return -(int)M9P_ERR_EIO;

    rc = pwos_host_rpc_peer_client_write_node(&client, owner_target, remote_path,
                                              data, data_len, out_written, deadline_ms);
    runtime_lock();
    if (rc == 0) g_host_rpc.status.remote_writes++;
    else { g_host_rpc.status.client_errors++; g_host_rpc.status.last_error = rc; }
    runtime_unlock();
    return rc;
#else
    (void)path; (void)data; (void)data_len; (void)out_written; (void)deadline_ms; return -1;
#endif
}

/* ========================================================================
 * 第十五节: 分布式推理接口
 * ======================================================================== */

int pwos_host_rpc_runtime_llm_submit(const char *hostname, const char *prompt,
                                     uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    if (!prompt || !prompt[0]) return -(int)M9P_ERR_EINVAL;

    if (!hostname || !hostname[0]) {
        uint16_t written = 0u;
        return pwos_dist_inference_service_write("/llm/prompt", (const uint8_t *)prompt,
                                                 (uint16_t)strlen(prompt), &written, deadline_ms);
    }

    runtime_lock();
    runtime_peer_t *peer = find_peer_hostname_locked(hostname);
    if (!peer || !peer->snapshot.ip[0]) { runtime_unlock(); return PWOS_SESSION_ERR_NO_ROUTE; }
    exchange_target_t target;
    snprintf(target.ip, sizeof(target.ip), "%s", peer->snapshot.ip);
    target.port = peer->snapshot.port;
    runtime_unlock();

    pwos_host_rpc_peer_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.io_ctx = &target; cfg.exchange = tcp_exchange;
    pwos_host_rpc_peer_client_t client;
    if (pwos_host_rpc_peer_client_init(&client, &cfg) != 0) return -(int)M9P_ERR_EIO;

    uint16_t written = 0u;
    int rc = pwos_host_rpc_peer_client_write_node(&client, "llm", "/prompt",
                                                  (const uint8_t *)prompt,
                                                  (uint16_t)strlen(prompt), &written, deadline_ms);
    runtime_lock();
    if (rc == 0) g_host_rpc.status.remote_writes++;
    else { g_host_rpc.status.client_errors++; g_host_rpc.status.last_error = rc; }
    runtime_unlock();
    return rc;
#else
    (void)hostname; (void)prompt; (void)deadline_ms; return -1;
#endif
}

int pwos_host_rpc_runtime_llm_result(const char *hostname, uint8_t *out,
                                     uint16_t *in_out_len, uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    if (!out || !in_out_len) return -(int)M9P_ERR_EINVAL;
    if (!hostname || !hostname[0])
        return pwos_dist_inference_service_read("/llm/result", out, in_out_len, deadline_ms);

    runtime_lock();
    runtime_peer_t *peer = find_peer_hostname_locked(hostname);
    if (!peer || !peer->snapshot.ip[0]) { runtime_unlock(); return PWOS_SESSION_ERR_NO_ROUTE; }
    exchange_target_t target;
    snprintf(target.ip, sizeof(target.ip), "%s", peer->snapshot.ip);
    target.port = peer->snapshot.port;
    runtime_unlock();

    pwos_host_rpc_peer_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.io_ctx = &target; cfg.exchange = tcp_exchange;
    pwos_host_rpc_peer_client_t client;
    if (pwos_host_rpc_peer_client_init(&client, &cfg) != 0) return -(int)M9P_ERR_EIO;

    int rc = pwos_host_rpc_peer_client_read_node(&client, "llm", "/result",
                                                 out, in_out_len, deadline_ms);
    runtime_lock();
    if (rc == 0) g_host_rpc.status.remote_reads++;
    else { g_host_rpc.status.client_errors++; g_host_rpc.status.last_error = rc; }
    runtime_unlock();
    return rc;
#else
    (void)hostname; (void)out; (void)in_out_len; (void)deadline_ms; return -1;
#endif
}

int pwos_host_rpc_runtime_llm_status(const char *hostname, uint8_t *out,
                                     uint16_t *in_out_len, uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    if (!out || !in_out_len) return -(int)M9P_ERR_EINVAL;
    if (!hostname || !hostname[0])
        return pwos_dist_inference_service_read("/llm/status", out, in_out_len, deadline_ms);

    runtime_lock();
    runtime_peer_t *peer = find_peer_hostname_locked(hostname);
    if (!peer || !peer->snapshot.ip[0]) { runtime_unlock(); return PWOS_SESSION_ERR_NO_ROUTE; }
    exchange_target_t target;
    snprintf(target.ip, sizeof(target.ip), "%s", peer->snapshot.ip);
    target.port = peer->snapshot.port;
    runtime_unlock();

    pwos_host_rpc_peer_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.io_ctx = &target; cfg.exchange = tcp_exchange;
    pwos_host_rpc_peer_client_t client;
    if (pwos_host_rpc_peer_client_init(&client, &cfg) != 0) return -(int)M9P_ERR_EIO;

    int rc = pwos_host_rpc_peer_client_read_node(&client, "llm", "/status",
                                                 out, in_out_len, deadline_ms);
    runtime_lock();
    if (rc == 0) g_host_rpc.status.remote_reads++;
    runtime_unlock();
    return rc;
#else
    (void)hostname; (void)out; (void)in_out_len; (void)deadline_ms; return -1;
#endif
}
