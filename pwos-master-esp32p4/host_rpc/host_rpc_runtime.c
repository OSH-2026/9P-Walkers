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

#define PWOS_HOST_RPC_LOCAL_PRIORITY 200u
#define PWOS_HOST_RPC_SERVER_STACK 8192u
#define PWOS_HOST_RPC_DISCOVERY_STACK 12288u
#define PWOS_HOST_RPC_TASK_PRIORITY 4u
#define PWOS_HOST_RPC_DISCOVERY_INTERVAL_MS 5000u
#define PWOS_HOST_RPC_DISCOVERY_QUERY_MS 1200u
#define PWOS_HOST_RPC_DEFAULT_DEADLINE_MS 1500u

typedef struct {
    uint8_t used;
    pwos_host_rpc_peer_snapshot_t snapshot;
} runtime_peer_t;

typedef struct {
    char ip[16];
    uint16_t port;
} exchange_target_t;

typedef struct {
    pwos_host_rpc_service_t service;
    pwos_host_election_t election;
    runtime_peer_t peers[PWOS_HOST_RPC_MAX_PEERS];
    pwos_host_rpc_topology_t topology;
    pwos_host_rpc_advertise_t local_advertise;
    pwos_host_rpc_runtime_status_t status;
    char current_remote_ip[16];
#ifdef ESP_PLATFORM
    SemaphoreHandle_t mutex;
    TaskHandle_t server_task;
    TaskHandle_t discovery_task;
#endif
} host_rpc_runtime_t;

static host_rpc_runtime_t g_host_rpc;

#ifdef ESP_PLATFORM
static const char *TAG = "pwos_host_rpc";

static void runtime_lock(void)
{
    (void)xSemaphoreTake(g_host_rpc.mutex, portMAX_DELAY);
}

static void runtime_unlock(void)
{
    (void)xSemaphoreGive(g_host_rpc.mutex);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t load_next_epoch(void)
{
    static const char *namespace_name = "pwos_host";
    static const char *epoch_key = "epoch";
    nvs_handle_t handle;
    uint32_t epoch = 0u;
    esp_err_t error;

    error = nvs_flash_init();
    if (error == ESP_OK) {
        error = nvs_open(namespace_name, NVS_READWRITE, &handle);
    }
    if (error == ESP_OK) {
        error = nvs_get_u32(handle, epoch_key, &epoch);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            epoch = 0u;
            error = ESP_OK;
        }
        if (error == ESP_OK) {
            ++epoch;
            if (epoch == 0u) epoch = 1u;
            error = nvs_set_u32(handle, epoch_key, epoch);
        }
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error == ESP_OK) return epoch;

    /* NVS 故障不能阻止 coordinator 启动，但日志必须保留降级原因。 */
    epoch = esp_random();
    if (epoch == 0u) epoch = 1u;
    ESP_LOGW(TAG, "NVS epoch unavailable: %s, fallback=%lu",
        esp_err_to_name(error), (unsigned long)epoch);
    return epoch;
}

static int uid_equal(const uint32_t lhs[3], const uint32_t rhs[3])
{
    return lhs[0] == rhs[0] && lhs[1] == rhs[1] && lhs[2] == rhs[2];
}

static void refresh_role_locked(void)
{
    g_host_rpc.status.local_role = g_host_rpc.election.local_role;
    memcpy(
        g_host_rpc.status.leader_uid,
        g_host_rpc.election.leader.uid,
        sizeof(g_host_rpc.status.leader_uid));
    g_host_rpc.local_advertise.role = g_host_rpc.election.local_role;
}

static runtime_peer_t *find_peer_uid_locked(const uint32_t uid[3])
{
    size_t i;

    for (i = 0u; i < PWOS_HOST_RPC_MAX_PEERS; ++i) {
        if (g_host_rpc.peers[i].used != 0u &&
            uid_equal(g_host_rpc.peers[i].snapshot.uid, uid)) {
            return &g_host_rpc.peers[i];
        }
    }
    return NULL;
}

static runtime_peer_t *find_peer_ip_locked(const char *ip)
{
    size_t i;

    for (i = 0u; i < PWOS_HOST_RPC_MAX_PEERS; ++i) {
        if (g_host_rpc.peers[i].used != 0u &&
            strcmp(g_host_rpc.peers[i].snapshot.ip, ip) == 0) {
            return &g_host_rpc.peers[i];
        }
    }
    return NULL;
}

static int upsert_peer_locked(
    const pwos_host_rpc_advertise_t *advertise,
    const char *ip)
{
    runtime_peer_t *peer = find_peer_uid_locked(advertise->uid);
    size_t i;

    if (uid_equal(advertise->uid, g_host_rpc.local_advertise.uid)) {
        return -1;
    }
    if (peer == NULL) {
        for (i = 0u; i < PWOS_HOST_RPC_MAX_PEERS; ++i) {
            if (g_host_rpc.peers[i].used == 0u) {
                peer = &g_host_rpc.peers[i];
                memset(peer, 0, sizeof(*peer));
                peer->used = 1u;
                peer->snapshot.used = 1u;
                ++g_host_rpc.status.peer_count;
                break;
            }
        }
    }
    if (peer == NULL) {
        return -1;
    }
    memcpy(peer->snapshot.uid, advertise->uid, sizeof(peer->snapshot.uid));
    peer->snapshot.epoch = advertise->epoch;
    peer->snapshot.priority = advertise->priority;
    peer->snapshot.role = advertise->role;
    peer->snapshot.port = advertise->rpc_port;
    peer->snapshot.last_seen_ms = now_ms();
    (void)snprintf(peer->snapshot.hostname, sizeof(peer->snapshot.hostname),
        "%s", advertise->hostname);
    if (ip != NULL && ip[0] != '\0') {
        (void)snprintf(peer->snapshot.ip, sizeof(peer->snapshot.ip), "%s", ip);
    }
    if (pwos_host_election_update_peer(
            &g_host_rpc.election,
            advertise->uid,
            advertise->epoch,
            advertise->priority,
            peer->snapshot.last_seen_ms) != 0) {
        return -1;
    }
    refresh_role_locked();
    return 0;
}

static int send_all(int socket_fd, const uint8_t *data, size_t len)
{
    size_t sent = 0u;

    while (sent < len) {
        int rc = send(socket_fd, data + sent, len - sent, 0);

        if (rc <= 0) return -1;
        sent += (size_t)rc;
    }
    return 0;
}

static int recv_all(int socket_fd, uint8_t *data, size_t len)
{
    size_t received = 0u;

    while (received < len) {
        int rc = recv(socket_fd, data + received, len - received, 0);

        if (rc <= 0) return -1;
        received += (size_t)rc;
    }
    return 0;
}

static void set_socket_timeout(int socket_fd, uint32_t deadline_ms)
{
    struct timeval timeout;

    if (deadline_ms == 0u) deadline_ms = PWOS_HOST_RPC_DEFAULT_DEADLINE_MS;
    timeout.tv_sec = (time_t)(deadline_ms / 1000u);
    timeout.tv_usec = (suseconds_t)((deadline_ms % 1000u) * 1000u);
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int tcp_exchange(
    void *ctx,
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_cap,
    size_t *out_response_len,
    uint32_t deadline_ms)
{
    const exchange_target_t *target = (const exchange_target_t *)ctx;
    struct sockaddr_in address;
    uint32_t body_len;
    int socket_fd;
    int rc = -(int)M9P_ERR_EIO;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(target->port);
    if (inet_pton(AF_INET, target->ip, &address.sin_addr) != 1) {
        return -(int)M9P_ERR_EINVAL;
    }
    socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) return rc;
    set_socket_timeout(socket_fd, deadline_ms);
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        send_all(socket_fd, request, request_len) != 0 ||
        response_cap < PWOS_HOST_RPC_PREFIX_LEN ||
        recv_all(socket_fd, response, PWOS_HOST_RPC_PREFIX_LEN) != 0) {
        if (errno == EAGAIN || errno == ETIMEDOUT) rc = PWOS_SESSION_ERR_DEADLINE;
        goto done;
    }
    body_len = pwos_host_rpc_body_len(response);
    if (body_len == 0u || body_len > response_cap - PWOS_HOST_RPC_PREFIX_LEN ||
        body_len > PWOS_HOST_RPC_MAX_FRAME_LEN - PWOS_HOST_RPC_PREFIX_LEN ||
        recv_all(socket_fd, response + PWOS_HOST_RPC_PREFIX_LEN, body_len) != 0) {
        goto done;
    }
    *out_response_len = PWOS_HOST_RPC_PREFIX_LEN + body_len;
    rc = 0;

done:
    (void)shutdown(socket_fd, SHUT_RDWR);
    (void)close(socket_fd);
    return rc;
}

static int call_address(
    const char *ip,
    uint16_t port,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status)
{
    pwos_host_rpc_peer_client_config_t config;
    pwos_host_rpc_peer_client_t client;
    exchange_target_t target;
    int rc;

    memset(&target, 0, sizeof(target));
    (void)snprintf(target.ip, sizeof(target.ip), "%s", ip);
    target.port = port;
    memset(&config, 0, sizeof(config));
    config.io_ctx = &target;
    config.exchange = tcp_exchange;
    if (pwos_host_rpc_peer_client_init(&client, &config) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }
    rc = pwos_host_rpc_peer_client_call(
        &client, service, method, payload, payload_len, deadline_ms,
        response, in_out_response_len, out_status);
    runtime_lock();
    ++g_host_rpc.status.client_calls;
    if (rc != 0 || *out_status != PWOS_HOST_RPC_STATUS_OK) {
        ++g_host_rpc.status.client_errors;
        g_host_rpc.status.last_error = rc != 0 ? rc :
            -(PWOS_HOST_RPC_REMOTE_ERROR_BASE + (int)*out_status);
    } else {
        g_host_rpc.status.last_error = 0;
    }
    runtime_unlock();
    return rc;
}

static int build_local_topology(pwos_host_rpc_topology_t *out)
{
    size_t i;

    memset(out, 0, sizeof(*out));
    runtime_lock();
    out->generation = g_host_rpc.topology.generation;
    runtime_unlock();
    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES &&
         out->node_count < PWOS_HOST_RPC_TOPOLOGY_MAX_NODES; ++i) {
        pwos_cluster_vfs_route_t route;
        pwos_host_rpc_topology_node_t *node;

        if (pwos_coordinator_runtime_get_route(i, &route) != 0 ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE) {
            continue;
        }
        node = &out->nodes[out->node_count++];
        (void)snprintf(node->global_target, sizeof(node->global_target), "%s", route.target);
        (void)snprintf(node->owner_target, sizeof(node->owner_target), "%s", route.target);
        runtime_lock();
        memcpy(node->owner_uid, g_host_rpc.local_advertise.uid, sizeof(node->owner_uid));
        runtime_unlock();
        memcpy(node->node_uid, route.uid, sizeof(node->node_uid));
        node->boot_id = route.boot_id;
    }
    return 0;
}

static int topology_name_used_locked(const char *name, const uint32_t node_uid[3])
{
    size_t i;

    for (i = 0u; i < g_host_rpc.topology.node_count; ++i) {
        const pwos_host_rpc_topology_node_t *node = &g_host_rpc.topology.nodes[i];

        if (strcmp(node->global_target, name) == 0 && !uid_equal(node->node_uid, node_uid)) {
            return 1;
        }
    }
    return 0;
}

static void allocate_global_name_locked(
    char out[PWOS_HOST_RPC_TARGET_CAP],
    const uint32_t node_uid[3])
{
    unsigned number;

    for (number = 1u; number < 10000u; ++number) {
        char candidate[PWOS_HOST_RPC_TARGET_CAP];

        (void)snprintf(candidate, sizeof(candidate), "mcu%u", number);
        if (!topology_name_used_locked(candidate, node_uid)) {
            (void)snprintf(out, PWOS_HOST_RPC_TARGET_CAP, "%s", candidate);
            return;
        }
    }
    out[0] = '\0';
}

static int topology_snapshot_has_node(
    const pwos_host_rpc_topology_t *snapshot,
    const uint32_t node_uid[3])
{
    size_t i;

    if (snapshot == NULL) return 0;
    for (i = 0u; i < snapshot->node_count; ++i) {
        if (uid_equal(snapshot->nodes[i].node_uid, node_uid)) return 1;
    }
    return 0;
}

static void prune_owner_locked(
    const uint32_t owner_uid[3],
    const pwos_host_rpc_topology_t *snapshot)
{
    size_t i = 0u;

    while (i < g_host_rpc.topology.node_count) {
        pwos_host_rpc_topology_node_t *node = &g_host_rpc.topology.nodes[i];

        if (!uid_equal(node->owner_uid, owner_uid) ||
            topology_snapshot_has_node(snapshot, node->node_uid)) {
            ++i;
            continue;
        }
        --g_host_rpc.topology.node_count;
        if (i < g_host_rpc.topology.node_count) {
            memmove(
                &g_host_rpc.topology.nodes[i],
                &g_host_rpc.topology.nodes[i + 1u],
                (g_host_rpc.topology.node_count - i) *
                    sizeof(g_host_rpc.topology.nodes[0]));
        }
        memset(&g_host_rpc.topology.nodes[g_host_rpc.topology.node_count],
            0, sizeof(g_host_rpc.topology.nodes[0]));
    }
    g_host_rpc.status.topology_nodes = g_host_rpc.topology.node_count;
}

static void filter_owner_topology(
    const pwos_host_rpc_topology_t *incoming,
    const uint32_t owner_uid[3],
    pwos_host_rpc_topology_t *out)
{
    size_t i;

    memset(out, 0, sizeof(*out));
    out->generation = incoming->generation;
    for (i = 0u; i < incoming->node_count; ++i) {
        if (!uid_equal(incoming->nodes[i].owner_uid, owner_uid)) continue;
        out->nodes[out->node_count++] = incoming->nodes[i];
    }
}

static void merge_topology_locked(
    const pwos_host_rpc_topology_t *incoming,
    uint8_t trust_global_names)
{
    size_t i;

    if (trust_global_names != 0u) {
        /* follower 对当前 leader 的全局命名采用整表快照，避免旧本地名造成二次改名。 */
        g_host_rpc.topology = *incoming;
        g_host_rpc.status.topology_nodes = g_host_rpc.topology.node_count;
        return;
    }
    for (i = 0u; i < incoming->node_count; ++i) {
        const pwos_host_rpc_topology_node_t *source = &incoming->nodes[i];
        pwos_host_rpc_topology_node_t *target = NULL;
        size_t j;

        for (j = 0u; j < g_host_rpc.topology.node_count; ++j) {
            if (uid_equal(g_host_rpc.topology.nodes[j].node_uid, source->node_uid)) {
                target = &g_host_rpc.topology.nodes[j];
                break;
            }
        }
        if (target == NULL) {
            if (g_host_rpc.topology.node_count >= PWOS_HOST_RPC_TOPOLOGY_MAX_NODES) continue;
            target = &g_host_rpc.topology.nodes[g_host_rpc.topology.node_count++];
            memset(target, 0, sizeof(*target));
        }
        memcpy(target->owner_uid, source->owner_uid, sizeof(target->owner_uid));
        memcpy(target->node_uid, source->node_uid, sizeof(target->node_uid));
        target->boot_id = source->boot_id;
        (void)snprintf(target->owner_target, sizeof(target->owner_target),
            "%s", source->owner_target);
        if (target->global_target[0] == '\0') {
            if (source->global_target[0] != '\0' &&
                !topology_name_used_locked(source->global_target, source->node_uid)) {
                (void)snprintf(target->global_target, sizeof(target->global_target),
                    "%s", source->global_target);
            } else {
                allocate_global_name_locked(target->global_target, source->node_uid);
            }
        }
    }
    ++g_host_rpc.topology.generation;
    g_host_rpc.status.topology_nodes = g_host_rpc.topology.node_count;
}

static void refresh_local_topology(void)
{
    pwos_host_rpc_topology_t local;

    (void)build_local_topology(&local);
    runtime_lock();
    prune_owner_locked(g_host_rpc.local_advertise.uid, &local);
    merge_topology_locked(&local, 0u);
    runtime_unlock();
}

static int service_read_node(
    void *ctx,
    const char *target,
    const char *path,
    uint8_t *data,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    char full_path[PWOS_HOST_RPC_TARGET_CAP + PWOS_HOST_RPC_PATH_CAP + 2u];

    (void)ctx;
    /* target == "llm" 时由分布式推理服务处理，不走 coordinator -> STM32 链路。 */
    if (strcmp(target, "llm") == 0) {
        if (snprintf(full_path, sizeof(full_path), "/llm%s", path) >=
            (int)sizeof(full_path)) return -(int)M9P_ERR_EMSIZE;
        return pwos_dist_inference_service_read(
            full_path, data, in_out_len, deadline_ms);
    }
    if (snprintf(full_path, sizeof(full_path), "/%s%s", target, path) >=
        (int)sizeof(full_path)) return -(int)M9P_ERR_EMSIZE;
    return pwos_coordinator_runtime_read_path(
        full_path, data, in_out_len, deadline_ms);
}

static int service_write_node(
    void *ctx,
    const char *target,
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    char full_path[PWOS_HOST_RPC_TARGET_CAP + PWOS_HOST_RPC_PATH_CAP + 2u];

    (void)ctx;
    /* target == "llm" 时由分布式推理服务处理，不走 coordinator -> STM32 链路。 */
    if (strcmp(target, "llm") == 0) {
        if (snprintf(full_path, sizeof(full_path), "/llm%s", path) >=
            (int)sizeof(full_path)) return -(int)M9P_ERR_EMSIZE;
        return pwos_dist_inference_service_write(
            full_path, data, data_len, out_written, deadline_ms);
    }
    if (snprintf(full_path, sizeof(full_path), "/%s%s", target, path) >=
        (int)sizeof(full_path)) return -(int)M9P_ERR_EMSIZE;
    return pwos_coordinator_runtime_write_path(
        full_path, data, data_len, out_written, deadline_ms);
}

static int service_advertise(
    void *ctx,
    const pwos_host_rpc_advertise_t *advertise)
{
    (void)ctx;
    runtime_lock();
    if (upsert_peer_locked(advertise, g_host_rpc.current_remote_ip) != 0) {
        runtime_unlock();
        return -(int)M9P_ERR_EBUSY;
    }
    runtime_unlock();
    return 0;
}

static int service_local_advertise(
    void *ctx,
    pwos_host_rpc_advertise_t *out_advertise)
{
    (void)ctx;
    runtime_lock();
    *out_advertise = g_host_rpc.local_advertise;
    runtime_unlock();
    return 0;
}

static int service_whoowns(
    void *ctx,
    const char *target,
    pwos_host_rpc_advertise_t *out_owner)
{
    size_t i;

    (void)ctx;
    runtime_lock();
    for (i = 0u; i < g_host_rpc.topology.node_count; ++i) {
        const pwos_host_rpc_topology_node_t *node = &g_host_rpc.topology.nodes[i];
        runtime_peer_t *peer;

        if (strcmp(node->global_target, target) != 0) continue;
        if (uid_equal(node->owner_uid, g_host_rpc.local_advertise.uid)) {
            *out_owner = g_host_rpc.local_advertise;
            runtime_unlock();
            return 0;
        }
        peer = find_peer_uid_locked(node->owner_uid);
        if (peer != NULL) {
            memset(out_owner, 0, sizeof(*out_owner));
            memcpy(out_owner->uid, peer->snapshot.uid, sizeof(out_owner->uid));
            out_owner->epoch = peer->snapshot.epoch;
            out_owner->priority = peer->snapshot.priority;
            out_owner->role = peer->snapshot.role;
            out_owner->rpc_port = peer->snapshot.port;
            (void)snprintf(out_owner->hostname, sizeof(out_owner->hostname),
                "%s", peer->snapshot.hostname);
            runtime_unlock();
            return 0;
        }
    }
    runtime_unlock();
    return -(int)M9P_ERR_ENOENT;
}

static int service_topology_sync(
    void *ctx,
    const pwos_host_rpc_topology_t *incoming,
    pwos_host_rpc_topology_t *out_current)
{
    runtime_peer_t *source;
    uint8_t trust = 0u;
    pwos_host_rpc_topology_t local;
    pwos_host_rpc_topology_t peer_snapshot;

    (void)ctx;
    (void)build_local_topology(&local);
    runtime_lock();
    source = find_peer_ip_locked(g_host_rpc.current_remote_ip);
    if (g_host_rpc.election.local_role == PWOS_HOST_ROLE_FOLLOWER &&
        source != NULL && uid_equal(
            source->snapshot.uid, g_host_rpc.election.leader.uid)) {
        trust = 1u;
    }
    prune_owner_locked(g_host_rpc.local_advertise.uid, &local);
    merge_topology_locked(&local, 0u);
    if (trust != 0u) {
        merge_topology_locked(incoming, 1u);
    } else if (source != NULL) {
        /* follower 只能提交自己拥有的节点，不能覆盖其他 owner 的记录。 */
        filter_owner_topology(incoming, source->snapshot.uid, &peer_snapshot);
        prune_owner_locked(source->snapshot.uid, &peer_snapshot);
        merge_topology_locked(&peer_snapshot, 0u);
    }
    if (g_host_rpc.election.local_role == PWOS_HOST_ROLE_LEADER) {
        *out_current = g_host_rpc.topology;
    } else {
        *out_current = local;
        out_current->generation = g_host_rpc.topology.generation;
    }
    runtime_unlock();
    return 0;
}

static void handle_client_socket(int client_fd, const char *remote_ip)
{
    uint8_t request[PWOS_HOST_RPC_MAX_FRAME_LEN];
    uint8_t response[PWOS_HOST_RPC_MAX_FRAME_LEN];
    uint32_t body_len;
    size_t response_len = 0u;

    set_socket_timeout(client_fd, 2000u);
    if (recv_all(client_fd, request, PWOS_HOST_RPC_PREFIX_LEN) != 0) goto fail;
    body_len = pwos_host_rpc_body_len(request);
    if (body_len == 0u || body_len > sizeof(request) - PWOS_HOST_RPC_PREFIX_LEN ||
        recv_all(client_fd, request + PWOS_HOST_RPC_PREFIX_LEN, body_len) != 0) goto fail;
    (void)snprintf(g_host_rpc.current_remote_ip,
        sizeof(g_host_rpc.current_remote_ip), "%s", remote_ip);
    if (pwos_host_rpc_service_handle(
            &g_host_rpc.service,
            request,
            PWOS_HOST_RPC_PREFIX_LEN + body_len,
            response,
            sizeof(response),
            &response_len) != 0 ||
        send_all(client_fd, response, response_len) != 0) goto fail;
    runtime_lock();
    ++g_host_rpc.status.server_requests;
    runtime_unlock();
    return;

fail:
    runtime_lock();
    ++g_host_rpc.status.server_errors;
    runtime_unlock();
}

static void server_task(void *arg)
{
    struct sockaddr_in address;
    int listen_fd;
    int reuse = 1;

    (void)arg;
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) goto fatal;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PWOS_HOST_RPC_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 4) != 0) {
        (void)close(listen_fd);
        goto fatal;
    }
    runtime_lock();
    g_host_rpc.status.server_started = 1u;
    runtime_unlock();
    ESP_LOGI(TAG, "server listening tcp/%u", PWOS_HOST_RPC_PORT);

    for (;;) {
        struct sockaddr_in remote;
        socklen_t remote_len = sizeof(remote);
        char remote_ip[16];
        int client_fd = accept(listen_fd, (struct sockaddr *)&remote, &remote_len);

        if (client_fd < 0) continue;
        (void)inet_ntop(AF_INET, &remote.sin_addr, remote_ip, sizeof(remote_ip));
        runtime_lock();
        ++g_host_rpc.status.accepted;
        runtime_unlock();
        handle_client_socket(client_fd, remote_ip);
        (void)shutdown(client_fd, SHUT_RDWR);
        (void)close(client_fd);
    }

fatal:
    runtime_lock();
    ++g_host_rpc.status.server_errors;
    g_host_rpc.status.last_error = -(int)M9P_ERR_EIO;
    runtime_unlock();
    vTaskDelete(NULL);
}

static int advertise_to(
    const char *ip,
    uint16_t port,
    pwos_host_rpc_advertise_t *out_remote)
{
    uint8_t args[256];
    uint8_t response[256];
    uint16_t args_len = 0u;
    uint16_t response_len = sizeof(response);
    uint16_t status;
    pwos_host_rpc_advertise_t local;
    int rc;

    runtime_lock();
    local = g_host_rpc.local_advertise;
    runtime_unlock();
    if (pwos_host_rpc_encode_advertise(
            &local, args, sizeof(args), &args_len) != 0) return -(int)M9P_ERR_EINVAL;
    rc = call_address(
        ip, port, "host", "advertise", args, args_len,
        PWOS_HOST_RPC_DEFAULT_DEADLINE_MS,
        response, &response_len, &status);
    if (rc != 0) return rc;
    if (status != PWOS_HOST_RPC_STATUS_OK ||
        pwos_host_rpc_decode_advertise(response, response_len, out_remote) != 0) {
        return -(int)M9P_ERR_EIO;
    }
    return 0;
}

static int topology_sync_to(const char *ip, uint16_t port, const uint32_t peer_uid[3])
{
    pwos_host_rpc_topology_t outgoing;
    pwos_host_rpc_topology_t incoming;
    pwos_host_rpc_topology_t peer_snapshot;
    uint8_t args[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint8_t response[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint16_t args_len = 0u;
    uint16_t response_len = sizeof(response);
    uint16_t status;
    int rc;

    refresh_local_topology();
    runtime_lock();
    outgoing = g_host_rpc.election.local_role == PWOS_HOST_ROLE_LEADER ?
        g_host_rpc.topology : (pwos_host_rpc_topology_t){0};
    runtime_unlock();
    if (outgoing.node_count == 0u) (void)build_local_topology(&outgoing);
    if (pwos_host_rpc_encode_topology(
            &outgoing, args, sizeof(args), &args_len) != 0) return -(int)M9P_ERR_EMSIZE;
    rc = call_address(
        ip, port, "topology", "sync", args, args_len,
        PWOS_HOST_RPC_DEFAULT_DEADLINE_MS,
        response, &response_len, &status);
    if (rc != 0) return rc;
    if (status != PWOS_HOST_RPC_STATUS_OK ||
        pwos_host_rpc_decode_topology(response, response_len, &incoming) != 0) {
        return -(int)M9P_ERR_EIO;
    }
    runtime_lock();
    if (uid_equal(peer_uid, g_host_rpc.election.leader.uid)) {
        merge_topology_locked(&incoming, 1u);
    } else {
        filter_owner_topology(&incoming, peer_uid, &peer_snapshot);
        prune_owner_locked(peer_uid, &peer_snapshot);
        merge_topology_locked(&peer_snapshot, 0u);
    }
    runtime_unlock();
    return 0;
}

static void process_mdns_result(mdns_result_t *result)
{
    mdns_ip_addr_t *address;
    pwos_host_rpc_advertise_t remote;
    char ip[16];
    int rc;

    if (result == NULL || result->hostname == NULL || result->port == 0u ||
        strcmp(result->hostname, g_host_rpc.status.hostname) == 0) return;
    for (address = result->addr; address != NULL; address = address->next) {
        if (address->addr.type == ESP_IPADDR_TYPE_V6) continue;
        (void)snprintf(ip, sizeof(ip), IPSTR, IP2STR(&address->addr.u_addr.ip4));
        memset(&remote, 0, sizeof(remote));
        rc = advertise_to(ip, result->port, &remote);
        runtime_lock();
        ++g_host_rpc.status.discovery_results;
        if (rc == 0 && upsert_peer_locked(&remote, ip) == 0) {
            ++g_host_rpc.status.advertise_ok;
        } else {
            ++g_host_rpc.status.advertise_fail;
            g_host_rpc.status.last_error = rc;
        }
        runtime_unlock();
        if (rc == 0) {
            rc = topology_sync_to(ip, remote.rpc_port, remote.uid);
            runtime_lock();
            if (rc == 0) ++g_host_rpc.status.topology_sync_ok;
            else {
                ++g_host_rpc.status.topology_sync_fail;
                g_host_rpc.status.last_error = rc;
            }
            runtime_unlock();
        }
        break;
    }
}

static void expire_peers(void)
{
    uint32_t now = now_ms();
    size_t i;

    runtime_lock();
    (void)pwos_host_election_expire(&g_host_rpc.election, now);
    for (i = 0u; i < PWOS_HOST_RPC_MAX_PEERS; ++i) {
        if (g_host_rpc.peers[i].used != 0u &&
            (uint32_t)(now - g_host_rpc.peers[i].snapshot.last_seen_ms) >=
                g_host_rpc.election.timeout_ms) {
            prune_owner_locked(g_host_rpc.peers[i].snapshot.uid, NULL);
            memset(&g_host_rpc.peers[i], 0, sizeof(g_host_rpc.peers[i]));
            if (g_host_rpc.status.peer_count > 0u) --g_host_rpc.status.peer_count;
        }
    }
    refresh_role_locked();
    runtime_unlock();
}

static void discovery_task(void *arg)
{
    (void)arg;
    runtime_lock();
    g_host_rpc.status.discovery_started = 1u;
    runtime_unlock();
    for (;;) {
        pwos_lan_runtime_status_t lan;
        mdns_result_t *results = NULL;
        mdns_result_t *result;

        pwos_lan_runtime_get_status(&lan);
        if (lan.has_ip != 0u && lan.mdns_ready != 0u) {
            esp_err_t error;

            runtime_lock();
            ++g_host_rpc.status.discovery_queries;
            runtime_unlock();
            error = mdns_query_ptr(
                "_pwos", "_tcp", PWOS_HOST_RPC_DISCOVERY_QUERY_MS,
                PWOS_HOST_RPC_MAX_PEERS + 1u, &results);
            if (error == ESP_OK) {
                for (result = results; result != NULL; result = result->next) {
                    process_mdns_result(result);
                }
            }
            if (results != NULL) mdns_query_results_free(results);
        }
        expire_peers();
        vTaskDelay(pdMS_TO_TICKS(PWOS_HOST_RPC_DISCOVERY_INTERVAL_MS));
    }
}

int pwos_host_rpc_runtime_start(void)
{
    pwos_lan_runtime_status_t lan;
    pwos_host_rpc_service_config_t service_config;
    uint32_t uid[3];
    uint32_t epoch;
    BaseType_t created;

    if (g_host_rpc.status.initialized != 0u) return 0;
    pwos_lan_runtime_get_status(&lan);
    if (lan.initialized == 0u || lan.hostname[0] == '\0') return -(int)M9P_ERR_EIO;
    memset(&g_host_rpc, 0, sizeof(g_host_rpc));
    g_host_rpc.mutex = xSemaphoreCreateMutex();
    if (g_host_rpc.mutex == NULL) return -(int)M9P_ERR_EBUSY;

    uid[0] = ((uint32_t)lan.mac[0] << 24) | ((uint32_t)lan.mac[1] << 16) |
        ((uint32_t)lan.mac[2] << 8) | lan.mac[3];
    uid[1] = ((uint32_t)lan.mac[4] << 24) | ((uint32_t)lan.mac[5] << 16) |
        0x5057u;
    uid[2] = 0x484F5354u; /* "HOST" */
    epoch = load_next_epoch();
    if (pwos_host_election_init(
            &g_host_rpc.election, uid, epoch,
            PWOS_HOST_RPC_LOCAL_PRIORITY, 0u) != 0) return -(int)M9P_ERR_EINVAL;
    g_host_rpc.local_advertise.uid[0] = uid[0];
    g_host_rpc.local_advertise.uid[1] = uid[1];
    g_host_rpc.local_advertise.uid[2] = uid[2];
    g_host_rpc.local_advertise.epoch = g_host_rpc.election.local.epoch;
    g_host_rpc.local_advertise.priority = PWOS_HOST_RPC_LOCAL_PRIORITY;
    g_host_rpc.local_advertise.role = PWOS_HOST_ROLE_LEADER;
    g_host_rpc.local_advertise.rpc_port = PWOS_HOST_RPC_PORT;
    (void)snprintf(g_host_rpc.local_advertise.hostname,
        sizeof(g_host_rpc.local_advertise.hostname), "%s", lan.hostname);

    memset(&service_config, 0, sizeof(service_config));
    service_config.read_node = service_read_node;
    service_config.write_node = service_write_node;
    service_config.advertise = service_advertise;
    service_config.local_advertise = service_local_advertise;
    service_config.whoowns = service_whoowns;
    service_config.topology_sync = service_topology_sync;
    if (pwos_host_rpc_service_init(&g_host_rpc.service, &service_config) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }
    g_host_rpc.status.initialized = 1u;
    g_host_rpc.status.local_role = PWOS_HOST_ROLE_LEADER;
    memcpy(g_host_rpc.status.local_uid, uid, sizeof(uid));
    g_host_rpc.status.local_epoch = g_host_rpc.local_advertise.epoch;
    g_host_rpc.status.local_priority = PWOS_HOST_RPC_LOCAL_PRIORITY;
    memcpy(g_host_rpc.status.leader_uid, uid, sizeof(uid));
    (void)snprintf(g_host_rpc.status.hostname, sizeof(g_host_rpc.status.hostname),
        "%s", lan.hostname);
    refresh_local_topology();
    if (pwos_lan_runtime_publish_host_rpc(
            uid, g_host_rpc.local_advertise.epoch,
            PWOS_HOST_RPC_LOCAL_PRIORITY, PWOS_HOST_RPC_PORT) != 0) {
        g_host_rpc.status.last_error = -(int)M9P_ERR_EIO;
    }

    created = xTaskCreate(server_task, "host_rpc_srv", PWOS_HOST_RPC_SERVER_STACK,
        NULL, PWOS_HOST_RPC_TASK_PRIORITY, &g_host_rpc.server_task);
    if (created != pdPASS) return -(int)M9P_ERR_EBUSY;
    created = xTaskCreate(discovery_task, "host_discovery", PWOS_HOST_RPC_DISCOVERY_STACK,
        NULL, PWOS_HOST_RPC_TASK_PRIORITY, &g_host_rpc.discovery_task);
    if (created != pdPASS) {
        vTaskDelete(g_host_rpc.server_task);
        g_host_rpc.server_task = NULL;
        return -(int)M9P_ERR_EBUSY;
    }
    ESP_LOGI(TAG, "started host=%s epoch=%lu uid=%08lx-%08lx-%08lx",
        g_host_rpc.status.hostname,
        (unsigned long)g_host_rpc.status.local_epoch,
        (unsigned long)uid[0], (unsigned long)uid[1], (unsigned long)uid[2]);
    return 0;
}

#else

int pwos_host_rpc_runtime_start(void)
{
    return -1;
}

#endif

void pwos_host_rpc_runtime_get_status(
    pwos_host_rpc_runtime_status_t *out_status)
{
    if (out_status == NULL) return;
#ifdef ESP_PLATFORM
    if (g_host_rpc.mutex != NULL) runtime_lock();
#endif
    *out_status = g_host_rpc.status;
#ifdef ESP_PLATFORM
    if (g_host_rpc.mutex != NULL) runtime_unlock();
#endif
}

int pwos_host_rpc_runtime_get_peer(
    size_t index,
    pwos_host_rpc_peer_snapshot_t *out_peer)
{
    if (out_peer == NULL || index >= PWOS_HOST_RPC_MAX_PEERS ||
        g_host_rpc.status.initialized == 0u) return -(int)M9P_ERR_EINVAL;
#ifdef ESP_PLATFORM
    runtime_lock();
#endif
    if (g_host_rpc.peers[index].used == 0u) {
#ifdef ESP_PLATFORM
        runtime_unlock();
#endif
        return -(int)M9P_ERR_ENOENT;
    }
    *out_peer = g_host_rpc.peers[index].snapshot;
#ifdef ESP_PLATFORM
    runtime_unlock();
#endif
    return 0;
}

int pwos_host_rpc_runtime_get_topology_node(
    size_t index,
    pwos_host_rpc_topology_node_t *out_node)
{
    if (out_node == NULL || g_host_rpc.status.initialized == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }
#ifdef ESP_PLATFORM
    runtime_lock();
#endif
    if (index >= g_host_rpc.topology.node_count) {
#ifdef ESP_PLATFORM
        runtime_unlock();
#endif
        return -(int)M9P_ERR_ENOENT;
    }
    *out_node = g_host_rpc.topology.nodes[index];
#ifdef ESP_PLATFORM
    runtime_unlock();
#endif
    return 0;
}

#ifdef ESP_PLATFORM
static int resolve_topology_path(
    const char *path,
    exchange_target_t *out_peer,
    char owner_target[PWOS_HOST_RPC_TARGET_CAP],
    char remote_path[PWOS_HOST_RPC_PATH_CAP],
    uint8_t *out_local_owner)
{
    const char *separator;
    char global_target[PWOS_HOST_RPC_TARGET_CAP];
    size_t target_len;
    size_t i;

    if (path == NULL || path[0] != '/' || out_peer == NULL ||
        out_local_owner == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    *out_local_owner = 0u;
    separator = strchr(path + 1u, '/');
    if (separator == NULL) return -(int)M9P_ERR_EINVAL;
    target_len = (size_t)(separator - (path + 1u));
    if (target_len == 0u || target_len >= sizeof(global_target) ||
        strlen(separator) >= PWOS_HOST_RPC_PATH_CAP) return -(int)M9P_ERR_EINVAL;
    memcpy(global_target, path + 1u, target_len);
    global_target[target_len] = '\0';
    (void)snprintf(remote_path, PWOS_HOST_RPC_PATH_CAP, "%s", separator);

    runtime_lock();
    for (i = 0u; i < g_host_rpc.topology.node_count; ++i) {
        const pwos_host_rpc_topology_node_t *node = &g_host_rpc.topology.nodes[i];
        runtime_peer_t *peer;

        if (strcmp(node->global_target, global_target) != 0) continue;
        (void)snprintf(owner_target, PWOS_HOST_RPC_TARGET_CAP, "%s", node->owner_target);
        if (uid_equal(node->owner_uid, g_host_rpc.local_advertise.uid)) {
            *out_local_owner = 1u;
            runtime_unlock();
            return 0;
        }
        peer = find_peer_uid_locked(node->owner_uid);
        if (peer == NULL || peer->snapshot.ip[0] == '\0') {
            runtime_unlock();
            return PWOS_SESSION_ERR_NO_ROUTE;
        }
        (void)snprintf(out_peer->ip, sizeof(out_peer->ip), "%s", peer->snapshot.ip);
        out_peer->port = peer->snapshot.port;
        runtime_unlock();
        return 0;
    }
    runtime_unlock();
    return PWOS_SESSION_ERR_NO_ROUTE;
}
#endif

int pwos_host_rpc_runtime_read_path(
    const char *path,
    uint8_t *data,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    exchange_target_t peer;
    pwos_host_rpc_peer_client_config_t config;
    pwos_host_rpc_peer_client_t client;
    char owner_target[PWOS_HOST_RPC_TARGET_CAP];
    char remote_path[PWOS_HOST_RPC_PATH_CAP];
    char local_path[PWOS_HOST_RPC_TARGET_CAP + PWOS_HOST_RPC_PATH_CAP + 2u];
    uint8_t local_owner;
    int rc;

    if (g_host_rpc.status.initialized == 0u) return PWOS_SESSION_ERR_NO_ROUTE;
    /* /llm/ 路径由分布式推理服务本地处理，不查拓扑表。 */
    if (strncmp(path, "/llm/", 5u) == 0) {
        return pwos_dist_inference_service_read(
            path, data, in_out_len, deadline_ms);
    }
    rc = resolve_topology_path(
        path, &peer, owner_target, remote_path, &local_owner);
    if (rc != 0) return rc;
    if (local_owner != 0u) {
        if (snprintf(local_path, sizeof(local_path), "/%s%s",
                owner_target, remote_path) >= (int)sizeof(local_path)) {
            return -(int)M9P_ERR_EMSIZE;
        }
        return pwos_coordinator_runtime_read_path(
            local_path, data, in_out_len, deadline_ms);
    }
    memset(&config, 0, sizeof(config));
    config.io_ctx = &peer;
    config.exchange = tcp_exchange;
    if (pwos_host_rpc_peer_client_init(&client, &config) != 0) return -(int)M9P_ERR_EIO;
    rc = pwos_host_rpc_peer_client_read_node(
        &client, owner_target, remote_path, data, in_out_len, deadline_ms);
    runtime_lock();
    if (rc == 0) ++g_host_rpc.status.remote_reads;
    else {
        ++g_host_rpc.status.client_errors;
        g_host_rpc.status.last_error = rc;
    }
    runtime_unlock();
    return rc;
#else
    (void)path; (void)data; (void)in_out_len; (void)deadline_ms;
    return -1;
#endif
}

int pwos_host_rpc_runtime_write_path(
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    exchange_target_t peer;
    pwos_host_rpc_peer_client_config_t config;
    pwos_host_rpc_peer_client_t client;
    char owner_target[PWOS_HOST_RPC_TARGET_CAP];
    char remote_path[PWOS_HOST_RPC_PATH_CAP];
    char local_path[PWOS_HOST_RPC_TARGET_CAP + PWOS_HOST_RPC_PATH_CAP + 2u];
    uint8_t local_owner;
    int rc;

    if (g_host_rpc.status.initialized == 0u) return PWOS_SESSION_ERR_NO_ROUTE;
    /* /llm/ 路径由分布式推理服务本地处理，不查拓扑表。 */
    if (strncmp(path, "/llm/", 5u) == 0) {
        return pwos_dist_inference_service_write(
            path, data, data_len, out_written, deadline_ms);
    }
    rc = resolve_topology_path(
        path, &peer, owner_target, remote_path, &local_owner);
    if (rc != 0) return rc;
    if (local_owner != 0u) {
        if (snprintf(local_path, sizeof(local_path), "/%s%s",
                owner_target, remote_path) >= (int)sizeof(local_path)) {
            return -(int)M9P_ERR_EMSIZE;
        }
        return pwos_coordinator_runtime_write_path(
            local_path, data, data_len, out_written, deadline_ms);
    }
    memset(&config, 0, sizeof(config));
    config.io_ctx = &peer;
    config.exchange = tcp_exchange;
    if (pwos_host_rpc_peer_client_init(&client, &config) != 0) return -(int)M9P_ERR_EIO;
    rc = pwos_host_rpc_peer_client_write_node(
        &client, owner_target, remote_path,
        data, data_len, out_written, deadline_ms);
    runtime_lock();
    if (rc == 0) ++g_host_rpc.status.remote_writes;
    else {
        ++g_host_rpc.status.client_errors;
        g_host_rpc.status.last_error = rc;
    }
    runtime_unlock();
    return rc;
#else
    (void)path; (void)data; (void)data_len; (void)out_written; (void)deadline_ms;
    return -1;
#endif
}

/* ===== 分布式推理接口 ===== */

#ifdef ESP_PLATFORM
static runtime_peer_t *find_peer_hostname_locked(const char *hostname)
{
    size_t i;

    for (i = 0u; i < PWOS_HOST_RPC_MAX_PEERS; ++i) {
        if (g_host_rpc.peers[i].used != 0u &&
            strcmp(g_host_rpc.peers[i].snapshot.hostname, hostname) == 0) {
            return &g_host_rpc.peers[i];
        }
    }
    return NULL;
}
#endif

int pwos_host_rpc_runtime_llm_submit(
    const char *hostname,
    const char *prompt,
    uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    if (prompt == NULL || prompt[0] == '\0') return -(int)M9P_ERR_EINVAL;

    /* hostname 为 NULL 或空 -> 本地推理 */
    if (hostname == NULL || hostname[0] == '\0') {
        uint16_t written = 0u;
        return pwos_dist_inference_service_write(
            "/llm/prompt", (const uint8_t *)prompt,
            (uint16_t)strlen(prompt), &written, deadline_ms);
    }

    /* 远程推理：按 hostname 查找 peer，通过 TCP 发送 write_node */
    {
        pwos_host_rpc_peer_client_config_t config;
        pwos_host_rpc_peer_client_t client;
        exchange_target_t target;
        runtime_peer_t *peer;
        uint16_t written = 0u;
        int rc;

        runtime_lock();
        peer = find_peer_hostname_locked(hostname);
        if (peer == NULL || peer->snapshot.ip[0] == '\0') {
            runtime_unlock();
            return PWOS_SESSION_ERR_NO_ROUTE;
        }
        memset(&target, 0, sizeof(target));
        (void)snprintf(target.ip, sizeof(target.ip), "%s", peer->snapshot.ip);
        target.port = peer->snapshot.port;
        runtime_unlock();

        memset(&config, 0, sizeof(config));
        config.io_ctx = &target;
        config.exchange = tcp_exchange;
        if (pwos_host_rpc_peer_client_init(&client, &config) != 0) {
            return -(int)M9P_ERR_EIO;
        }
        rc = pwos_host_rpc_peer_client_write_node(
            &client, "llm", "/prompt",
            (const uint8_t *)prompt, (uint16_t)strlen(prompt),
            &written, deadline_ms);
        runtime_lock();
        if (rc == 0) ++g_host_rpc.status.remote_writes;
        else { ++g_host_rpc.status.client_errors; g_host_rpc.status.last_error = rc; }
        runtime_unlock();
        return rc;
    }
#else
    (void)hostname; (void)prompt; (void)deadline_ms;
    return -1;
#endif
}

int pwos_host_rpc_runtime_llm_result(
    const char *hostname,
    uint8_t *out,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    if (out == NULL || in_out_len == NULL) return -(int)M9P_ERR_EINVAL;

    if (hostname == NULL || hostname[0] == '\0') {
        return pwos_dist_inference_service_read(
            "/llm/result", out, in_out_len, deadline_ms);
    }

    {
        pwos_host_rpc_peer_client_config_t config;
        pwos_host_rpc_peer_client_t client;
        exchange_target_t target;
        runtime_peer_t *peer;
        int rc;

        runtime_lock();
        peer = find_peer_hostname_locked(hostname);
        if (peer == NULL || peer->snapshot.ip[0] == '\0') {
            runtime_unlock();
            return PWOS_SESSION_ERR_NO_ROUTE;
        }
        memset(&target, 0, sizeof(target));
        (void)snprintf(target.ip, sizeof(target.ip), "%s", peer->snapshot.ip);
        target.port = peer->snapshot.port;
        runtime_unlock();

        memset(&config, 0, sizeof(config));
        config.io_ctx = &target;
        config.exchange = tcp_exchange;
        if (pwos_host_rpc_peer_client_init(&client, &config) != 0) {
            return -(int)M9P_ERR_EIO;
        }
        rc = pwos_host_rpc_peer_client_read_node(
            &client, "llm", "/result", out, in_out_len, deadline_ms);
        runtime_lock();
        if (rc == 0) ++g_host_rpc.status.remote_reads;
        else { ++g_host_rpc.status.client_errors; g_host_rpc.status.last_error = rc; }
        runtime_unlock();
        return rc;
    }
#else
    (void)hostname; (void)out; (void)in_out_len; (void)deadline_ms;
    return -1;
#endif
}

int pwos_host_rpc_runtime_llm_status(
    const char *hostname,
    uint8_t *out,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
#ifdef ESP_PLATFORM
    if (out == NULL || in_out_len == NULL) return -(int)M9P_ERR_EINVAL;

    if (hostname == NULL || hostname[0] == '\0') {
        return pwos_dist_inference_service_read(
            "/llm/status", out, in_out_len, deadline_ms);
    }

    {
        pwos_host_rpc_peer_client_config_t config;
        pwos_host_rpc_peer_client_t client;
        exchange_target_t target;
        runtime_peer_t *peer;
        int rc;

        runtime_lock();
        peer = find_peer_hostname_locked(hostname);
        if (peer == NULL || peer->snapshot.ip[0] == '\0') {
            runtime_unlock();
            return PWOS_SESSION_ERR_NO_ROUTE;
        }
        memset(&target, 0, sizeof(target));
        (void)snprintf(target.ip, sizeof(target.ip), "%s", peer->snapshot.ip);
        target.port = peer->snapshot.port;
        runtime_unlock();

        memset(&config, 0, sizeof(config));
        config.io_ctx = &target;
        config.exchange = tcp_exchange;
        if (pwos_host_rpc_peer_client_init(&client, &config) != 0) {
            return -(int)M9P_ERR_EIO;
        }
        rc = pwos_host_rpc_peer_client_read_node(
            &client, "llm", "/status", out, in_out_len, deadline_ms);
        runtime_lock();
        if (rc == 0) ++g_host_rpc.status.remote_reads;
        runtime_unlock();
        return rc;
    }
#else
    (void)hostname; (void)out; (void)in_out_len; (void)deadline_ms;
    return -1;
#endif
}

