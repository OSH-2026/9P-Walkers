#include "host_observability.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "http_server.h"
#include "host_rpc_runtime.h"
#include "lan_runtime.h"
#include "pwos_coordinator_runtime.h"
#include "websocket_console.h"

#define PWOS_HOST_PERM_READ 0x01u
#define PWOS_HOST_QID_BASE 0x7000u

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len;
} text_buffer_t;

typedef struct {
    const char *path;
    const char *name;
    uint8_t directory;
    uint32_t object_id;
} host_node_t;

static const host_node_t g_host_nodes[] = {
    {"/host", "host", 1u, PWOS_HOST_QID_BASE},
    {"/host/sys", "sys", 1u, PWOS_HOST_QID_BASE + 1u},
    {"/host/sys/health", "health", 0u, PWOS_HOST_QID_BASE + 2u},
    {"/host/sys/links", "links", 0u, PWOS_HOST_QID_BASE + 3u},
    {"/host/sys/topology", "topology", 0u, PWOS_HOST_QID_BASE + 4u},
    {"/host/sys/routes", "routes", 0u, PWOS_HOST_QID_BASE + 5u},
    {"/host/sys/sessions", "sessions", 0u, PWOS_HOST_QID_BASE + 6u},
    {"/host/sys/web", "web", 0u, PWOS_HOST_QID_BASE + 7u},
    {"/host/sys/log", "log", 0u, PWOS_HOST_QID_BASE + 8u},
    {"/host/sys/jobs", "jobs", 0u, PWOS_HOST_QID_BASE + 9u},
    {"/host/sys/hosts", "hosts", 0u, PWOS_HOST_QID_BASE + 10u},
};

static void text_append(text_buffer_t *buffer, const char *format, ...)
{
    va_list args;
    int written;

    if (buffer == NULL || buffer->data == NULL || buffer->cap == 0u ||
        buffer->len >= buffer->cap) {
        return;
    }
    va_start(args, format);
    written = vsnprintf(
        (char *)buffer->data + buffer->len,
        buffer->cap - buffer->len,
        format,
        args);
    va_end(args);
    if (written < 0) {
        return;
    }
    if ((size_t)written >= buffer->cap - buffer->len) {
        buffer->len = buffer->cap;
    } else {
        buffer->len += (size_t)written;
    }
}

static const char *route_state_name(pwos_cluster_vfs_route_state_t state)
{
    switch (state) {
    case PWOS_CLUSTER_VFS_ROUTE_OFFLINE:
        return "offline";
    case PWOS_CLUSTER_VFS_ROUTE_NEW:
        return "new";
    case PWOS_CLUSTER_VFS_ROUTE_ATTACHED:
        return "attached";
    case PWOS_CLUSTER_VFS_ROUTE_EMPTY:
    default:
        return "empty";
    }
}

static const host_node_t *find_node(const char *path)
{
    size_t i;

    for (i = 0u; i < sizeof(g_host_nodes) / sizeof(g_host_nodes[0]); ++i) {
        if (strcmp(path, g_host_nodes[i].path) == 0) {
            return &g_host_nodes[i];
        }
    }
    return NULL;
}

static void render_health(text_buffer_t *output)
{
    pwos_coordinator_runtime_stats_t runtime;
    pwos_session_manager_stats_t sessions;
    pwos_cluster_vfs_stats_t vfs;
    pwos_rpc_client_stats_t rpc;
    pwos_job_manager_stats_t jobs;
    pwos_host_rpc_runtime_status_t hosts;

    memset(&runtime, 0, sizeof(runtime));
    memset(&sessions, 0, sizeof(sessions));
    memset(&vfs, 0, sizeof(vfs));
    memset(&rpc, 0, sizeof(rpc));
    memset(&jobs, 0, sizeof(jobs));
    memset(&hosts, 0, sizeof(hosts));
    pwos_coordinator_runtime_get_stats(&runtime);
    pwos_coordinator_runtime_get_session_stats(&sessions);
    pwos_coordinator_runtime_get_vfs_stats(&vfs);
    pwos_coordinator_runtime_get_rpc_stats(&rpc);
    pwos_coordinator_runtime_get_job_stats(&jobs);
    pwos_host_rpc_runtime_get_status(&hosts);
    text_append(output, "status=ok\n");
    text_append(output, "uptime_ms=%llu\n",
        (unsigned long long)(esp_timer_get_time() / 1000));
    text_append(output, "free_heap=%lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    text_append(output, "nodes=%u\n", runtime.node_count);
    text_append(output, "link_rx_frames=%lu\n", (unsigned long)runtime.rx_frames);
    text_append(output, "link_tx_frames=%lu\n", (unsigned long)runtime.tx_frames);
    text_append(output, "link_parse_errors=%lu\n",
        (unsigned long)runtime.rx_parse_errors);
    text_append(output, "link_tx_errors=%lu\n", (unsigned long)runtime.tx_errors);
    text_append(output, "mini9p_probe_ok=%lu\n",
        (unsigned long)runtime.mini9p_probe_ok);
    text_append(output, "mini9p_probe_fail=%lu\n",
        (unsigned long)runtime.mini9p_probe_fail);
    text_append(output, "session_deadlines=%lu\n",
        (unsigned long)sessions.deadline_errors);
    text_append(output, "rpc_calls=%lu/%lu rpc_streams=%lu/%lu rpc_deadlines=%lu\n",
        (unsigned long)rpc.unary_rx,
        (unsigned long)rpc.unary_tx,
        (unsigned long)rpc.stream_rx,
        (unsigned long)rpc.stream_tx,
        (unsigned long)rpc.deadline_errors);
    text_append(output, "jobs_submitted=%lu jobs_lost=%lu jobs_transport_errors=%lu\n",
        (unsigned long)jobs.submitted,
        (unsigned long)jobs.lost,
        (unsigned long)jobs.transport_errors);
    text_append(output, "host_role=%s host_peers=%u host_nodes=%u host_rpc_errors=%lu\n",
        pwos_host_role_name(hosts.local_role),
        hosts.peer_count,
        hosts.topology_nodes,
        (unsigned long)(hosts.server_errors + hosts.client_errors));
    text_append(output, "vfs_last_error=%ld\n", (long)vfs.last_error);
}

static void render_links(text_buffer_t *output)
{
    pwos_coordinator_runtime_stats_t runtime;

    memset(&runtime, 0, sizeof(runtime));
    pwos_coordinator_runtime_get_stats(&runtime);
    text_append(output,
        "port=uart%u baud=%u rx_bytes=%lu rx_frames=%lu parse_errors=%lu "
        "tx_bytes=%lu tx_frames=%lu tx_errors=%lu last_rx_tick=%lu last_tx_tick=%lu\n",
        runtime.uart_port,
        PWOS_COORDINATOR_UART_BAUD_RATE,
        (unsigned long)runtime.rx_bytes,
        (unsigned long)runtime.rx_frames,
        (unsigned long)runtime.rx_parse_errors,
        (unsigned long)runtime.tx_bytes,
        (unsigned long)runtime.tx_frames,
        (unsigned long)runtime.tx_errors,
        (unsigned long)runtime.last_rx_tick,
        (unsigned long)runtime.last_tx_tick);
}

static void render_topology(text_buffer_t *output)
{
    size_t i;

    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        pwos_host_node_entry_t node;

        if (pwos_coordinator_runtime_get_node(i, &node) != 0) {
            continue;
        }
        text_append(output,
            "addr=%u upstream=%u uid=%08lx-%08lx-%08lx boot=%lu lease_epoch=%lu lease_ms=%lu\n",
            node.addr,
            node.upstream_port,
            (unsigned long)node.uid[0],
            (unsigned long)node.uid[1],
            (unsigned long)node.uid[2],
            (unsigned long)node.boot_id,
            (unsigned long)node.lease_epoch,
            (unsigned long)node.lease_ms);
    }
    if (output->len == 0u) {
        text_append(output, "(empty)\n");
    }
}

static void render_routes(text_buffer_t *output)
{
    size_t i;

    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        pwos_cluster_vfs_route_t route;

        if (pwos_coordinator_runtime_get_route(i, &route) != 0) {
            continue;
        }
        text_append(output,
            "target=%s addr=%u state=%s generation=%lu boot=%lu\n",
            route.target,
            route.addr,
            route_state_name(route.state),
            (unsigned long)route.generation,
            (unsigned long)route.boot_id);
    }
    if (output->len == 0u) {
        text_append(output, "(empty)\n");
    }
}

static void render_sessions(text_buffer_t *output)
{
    pwos_session_manager_stats_t stats;
    pwos_rpc_client_stats_t rpc;
    size_t i;

    memset(&stats, 0, sizeof(stats));
    memset(&rpc, 0, sizeof(rpc));
    pwos_coordinator_runtime_get_session_stats(&stats);
    pwos_coordinator_runtime_get_rpc_stats(&rpc);
    text_append(output,
        "total tx=%lu rx=%lu attach=%lu/%lu deadline=%lu no_route=%lu "
        "queue_full=%lu stale=%lu pending_peak=%lu unmatched=%lu stream_parts=%lu stream_done=%lu\n",
        (unsigned long)stats.tx_requests,
        (unsigned long)stats.rx_responses,
        (unsigned long)stats.attach_ok,
        (unsigned long)stats.attach_fail,
        (unsigned long)stats.deadline_errors,
        (unsigned long)stats.no_route_errors,
        (unsigned long)stats.queue_full_errors,
        (unsigned long)stats.stale_boot_errors,
        (unsigned long)stats.pending_peak,
        (unsigned long)stats.pending_unmatched,
        (unsigned long)stats.stream_parts_delivered,
        (unsigned long)stats.stream_completed);
    text_append(output,
        "rpc unary=%lu/%lu stream=%lu/%lu chunks=%lu oneway=%lu cancel=%lu deadline=%lu malformed=%lu remote_error=%lu last_call=%u last_status=%s(%u) last_error=%ld\n",
        (unsigned long)rpc.unary_rx,
        (unsigned long)rpc.unary_tx,
        (unsigned long)rpc.stream_rx,
        (unsigned long)rpc.stream_tx,
        (unsigned long)rpc.stream_chunks_rx,
        (unsigned long)rpc.oneway_tx,
        (unsigned long)rpc.cancel_tx,
        (unsigned long)rpc.deadline_errors,
        (unsigned long)rpc.malformed_responses,
        (unsigned long)rpc.remote_errors,
        rpc.last_call_id,
        pwos_rpc_status_name(rpc.last_status),
        rpc.last_status,
        (long)rpc.last_error);

    for (i = 0u; i < PWOS_SESSION_MANAGER_MAX_SESSIONS; ++i) {
        pwos_session_snapshot_t session;

        if (pwos_coordinator_runtime_get_session(i, &session) != 0) {
            continue;
        }
        text_append(output,
            "addr=%u attached=%u resetting=%u boot=%lu tx=%lu rx=%lu "
            "deadline=%lu no_route=%lu queue_full=%lu io=%lu tags=%u/%u\n",
            session.addr,
            session.attached,
            session.resetting,
            (unsigned long)session.boot_id,
            (unsigned long)session.tx_requests,
            (unsigned long)session.rx_responses,
            (unsigned long)session.deadline_errors,
            (unsigned long)session.no_route_errors,
            (unsigned long)session.queue_full_errors,
            (unsigned long)session.io_errors,
            session.last_tx_tag,
            session.last_rx_tag);
    }
}

static void render_jobs(text_buffer_t *output)
{
    pwos_job_manager_stats_t stats;
    size_t i;
    size_t count = 0u;

    memset(&stats, 0, sizeof(stats));
    pwos_coordinator_runtime_get_job_stats(&stats);
    text_append(output,
        "submitted=%lu status=%lu result=%lu cancelled=%lu retried=%lu "
        "lost=%lu transport_errors=%lu remote_errors=%lu reused=%lu last_id=%lu last_error=%ld\n",
        (unsigned long)stats.submitted,
        (unsigned long)stats.status_requests,
        (unsigned long)stats.result_requests,
        (unsigned long)stats.cancelled,
        (unsigned long)stats.retried,
        (unsigned long)stats.lost,
        (unsigned long)stats.transport_errors,
        (unsigned long)stats.remote_errors,
        (unsigned long)stats.slots_reused,
        (unsigned long)stats.last_host_job_id,
        (long)stats.last_error);
    for (i = 0u; i < PWOS_JOB_MANAGER_MAX_JOBS; ++i) {
        pwos_job_entry_t job;

        if (pwos_coordinator_runtime_get_job(i, &job) != 0) {
            continue;
        }
        text_append(output,
            "id=%lu target=%s addr=%u boot=%lu remote=%lu kernel=%s state=%s "
            "progress=%u.%u%% result=%lu status=%s(%u) error=%ld\n",
            (unsigned long)job.host_job_id,
            job.target,
            job.addr,
            (unsigned long)job.boot_id,
            (unsigned long)job.remote_job_id,
            pwos_job_kernel_name(job.kernel),
            pwos_job_state_name(job.state),
            job.progress_permille / 10u,
            job.progress_permille % 10u,
            (unsigned long)job.result_len,
            pwos_job_status_name(job.remote_status),
            job.remote_status,
            (long)job.last_error);
        ++count;
    }
    if (count == 0u) {
        text_append(output, "(empty)\n");
    }
}

static void render_hosts(text_buffer_t *output)
{
    pwos_host_rpc_runtime_status_t status;
    size_t i;

    memset(&status, 0, sizeof(status));
    pwos_host_rpc_runtime_get_status(&status);
    text_append(output,
        "host=%s initialized=%u server=%u discovery=%u role=%s "
        "uid=%08lx-%08lx-%08lx epoch=%lu priority=%u "
        "leader=%08lx-%08lx-%08lx peers=%u nodes=%u\n",
        status.hostname[0] == '\0' ? "-" : status.hostname,
        status.initialized,
        status.server_started,
        status.discovery_started,
        pwos_host_role_name(status.local_role),
        (unsigned long)status.local_uid[0],
        (unsigned long)status.local_uid[1],
        (unsigned long)status.local_uid[2],
        (unsigned long)status.local_epoch,
        status.local_priority,
        (unsigned long)status.leader_uid[0],
        (unsigned long)status.leader_uid[1],
        (unsigned long)status.leader_uid[2],
        status.peer_count,
        status.topology_nodes);
    text_append(output,
        "server accepted=%lu requests=%lu errors=%lu discovery=%lu/%lu "
        "advertise=%lu/%lu sync=%lu/%lu client=%lu errors=%lu "
        "remote_io=%lu/%lu last_error=%ld\n",
        (unsigned long)status.accepted,
        (unsigned long)status.server_requests,
        (unsigned long)status.server_errors,
        (unsigned long)status.discovery_results,
        (unsigned long)status.discovery_queries,
        (unsigned long)status.advertise_ok,
        (unsigned long)status.advertise_fail,
        (unsigned long)status.topology_sync_ok,
        (unsigned long)status.topology_sync_fail,
        (unsigned long)status.client_calls,
        (unsigned long)status.client_errors,
        (unsigned long)status.remote_reads,
        (unsigned long)status.remote_writes,
        (long)status.last_error);
    for (i = 0u; i < PWOS_HOST_RPC_MAX_PEERS; ++i) {
        pwos_host_rpc_peer_snapshot_t peer;

        if (pwos_host_rpc_runtime_get_peer(i, &peer) != 0) continue;
        text_append(output,
            "peer host=%s ip=%s port=%u role=%s uid=%08lx-%08lx-%08lx "
            "epoch=%lu priority=%u last_seen=%lu\n",
            peer.hostname,
            peer.ip,
            peer.port,
            pwos_host_role_name(peer.role),
            (unsigned long)peer.uid[0],
            (unsigned long)peer.uid[1],
            (unsigned long)peer.uid[2],
            (unsigned long)peer.epoch,
            peer.priority,
            (unsigned long)peer.last_seen_ms);
    }
    for (i = 0u; i < PWOS_HOST_RPC_TOPOLOGY_MAX_NODES; ++i) {
        pwos_host_rpc_topology_node_t node;

        if (pwos_host_rpc_runtime_get_topology_node(i, &node) != 0) break;
        text_append(output,
            "node global=%s owner_target=%s owner=%08lx-%08lx-%08lx "
            "uid=%08lx-%08lx-%08lx boot=%lu\n",
            node.global_target,
            node.owner_target,
            (unsigned long)node.owner_uid[0],
            (unsigned long)node.owner_uid[1],
            (unsigned long)node.owner_uid[2],
            (unsigned long)node.node_uid[0],
            (unsigned long)node.node_uid[1],
            (unsigned long)node.node_uid[2],
            (unsigned long)node.boot_id);
    }
}

static void render_web(text_buffer_t *output)
{
    pwos_lan_runtime_status_t lan;
    pwos_http_server_status_t http;
    pwos_websocket_console_status_t websocket;

    memset(&lan, 0, sizeof(lan));
    memset(&http, 0, sizeof(http));
    memset(&websocket, 0, sizeof(websocket));
    pwos_lan_runtime_get_status(&lan);
    pwos_http_server_get_status(&http);
    pwos_websocket_console_get_status(&websocket);
    text_append(output,
        "lan host=%s initialized=%u started=%u link=%u dhcp=%u ip=%s netmask=%s gateway=%s mdns=%u host_rpc_mdns=%u last_error=%ld\n",
        lan.hostname[0] == '\0' ? "-" : lan.hostname,
        lan.initialized,
        lan.started,
        lan.link_up,
        lan.has_ip,
        lan.ip[0] == '\0' ? "-" : lan.ip,
        lan.netmask[0] == '\0' ? "-" : lan.netmask,
        lan.gateway[0] == '\0' ? "-" : lan.gateway,
        lan.mdns_ready,
        lan.host_rpc_mdns_ready,
        (long)lan.last_error);
    text_append(output,
        "http started=%u port=%u root=%lu health=%lu ws_requests=%lu errors=%lu\n",
        http.started,
        http.port,
        (unsigned long)http.root_requests,
        (unsigned long)http.health_requests,
        (unsigned long)http.websocket_requests,
        (unsigned long)http.handler_errors);
    text_append(output,
        "websocket clients=%u connected=%lu disconnected=%lu commands=%lu/%lu "
        "failed=%lu queue_full=%lu send_failures=%lu stale=%lu\n",
        websocket.clients,
        (unsigned long)websocket.connected,
        (unsigned long)websocket.disconnected,
        (unsigned long)websocket.commands_completed,
        (unsigned long)websocket.commands_received,
        (unsigned long)websocket.commands_failed,
        (unsigned long)websocket.queue_full,
        (unsigned long)websocket.send_failures,
        (unsigned long)websocket.stale_results);
}

static void render_log(text_buffer_t *output)
{
    pwos_coordinator_runtime_stats_t runtime;
    pwos_session_manager_stats_t sessions;
    pwos_rpc_client_stats_t rpc;
    pwos_job_manager_stats_t jobs;
    pwos_host_rpc_runtime_status_t hosts;

    memset(&runtime, 0, sizeof(runtime));
    memset(&sessions, 0, sizeof(sessions));
    memset(&rpc, 0, sizeof(rpc));
    memset(&jobs, 0, sizeof(jobs));
    memset(&hosts, 0, sizeof(hosts));
    pwos_coordinator_runtime_get_stats(&runtime);
    pwos_coordinator_runtime_get_session_stats(&sessions);
    pwos_coordinator_runtime_get_rpc_stats(&rpc);
    pwos_coordinator_runtime_get_job_stats(&jobs);
    pwos_host_rpc_runtime_get_status(&hosts);
    text_append(output,
        "rx_frames=%lu tx_frames=%lu parse_errors=%lu tx_errors=%lu "
        "register=%lu assign=%lu renew=%lu route=%lu data_rx=%lu\n",
        (unsigned long)runtime.rx_frames,
        (unsigned long)runtime.tx_frames,
        (unsigned long)runtime.rx_parse_errors,
        (unsigned long)runtime.tx_errors,
        (unsigned long)runtime.register_rx,
        (unsigned long)runtime.assign_tx,
        (unsigned long)runtime.lease_renew_rx,
        (unsigned long)runtime.route_update_tx,
        (unsigned long)runtime.data_rx);
    text_append(output,
        "mini9p_tx=%lu mini9p_rx=%lu pending_delivered=%lu pending_unmatched=%lu "
        "deadline=%lu queue_full=%lu stale_boot=%lu io=%lu\n",
        (unsigned long)runtime.mini9p_tx,
        (unsigned long)runtime.mini9p_rx,
        (unsigned long)sessions.pending_delivered,
        (unsigned long)sessions.pending_unmatched,
        (unsigned long)sessions.deadline_errors,
        (unsigned long)sessions.queue_full_errors,
        (unsigned long)sessions.stale_boot_errors,
        (unsigned long)sessions.io_errors);
    text_append(output,
        "rpc_tx=%lu rpc_rx=%lu malformed=%lu unary=%lu/%lu stream=%lu/%lu chunks=%lu oneway=%lu cancel=%lu deadline=%lu remote_error=%lu last_error=%ld\n",
        (unsigned long)runtime.rpc_tx,
        (unsigned long)runtime.rpc_rx,
        (unsigned long)runtime.rpc_malformed,
        (unsigned long)rpc.unary_rx,
        (unsigned long)rpc.unary_tx,
        (unsigned long)rpc.stream_rx,
        (unsigned long)rpc.stream_tx,
        (unsigned long)rpc.stream_chunks_rx,
        (unsigned long)rpc.oneway_tx,
        (unsigned long)rpc.cancel_tx,
        (unsigned long)rpc.deadline_errors,
        (unsigned long)rpc.remote_errors,
        (long)rpc.last_error);
    text_append(output,
        "job_tx=%lu job_rx=%lu malformed=%lu submitted=%lu status=%lu result=%lu "
        "cancelled=%lu retried=%lu lost=%lu transport_error=%lu remote_error=%lu last_error=%ld\n",
        (unsigned long)runtime.job_tx,
        (unsigned long)runtime.job_rx,
        (unsigned long)runtime.job_malformed,
        (unsigned long)jobs.submitted,
        (unsigned long)jobs.status_requests,
        (unsigned long)jobs.result_requests,
        (unsigned long)jobs.cancelled,
        (unsigned long)jobs.retried,
        (unsigned long)jobs.lost,
        (unsigned long)jobs.transport_errors,
        (unsigned long)jobs.remote_errors,
        (long)jobs.last_error);
    text_append(output,
        "host_rpc role=%s peers=%u nodes=%u accepted=%lu requests=%lu server_error=%lu "
        "discovery=%lu/%lu advertise=%lu/%lu sync=%lu/%lu client=%lu/%lu last_error=%ld\n",
        pwos_host_role_name(hosts.local_role),
        hosts.peer_count,
        hosts.topology_nodes,
        (unsigned long)hosts.accepted,
        (unsigned long)hosts.server_requests,
        (unsigned long)hosts.server_errors,
        (unsigned long)hosts.discovery_results,
        (unsigned long)hosts.discovery_queries,
        (unsigned long)hosts.advertise_ok,
        (unsigned long)hosts.advertise_fail,
        (unsigned long)hosts.topology_sync_ok,
        (unsigned long)hosts.topology_sync_fail,
        (unsigned long)hosts.client_calls,
        (unsigned long)hosts.client_errors,
        (long)hosts.last_error);
}

int pwos_host_observability_read(
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len)
{
    const host_node_t *node;
    text_buffer_t output;

    if (path == NULL || buf == NULL || in_out_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }
    if (node->directory != 0u) {
        return -(int)M9P_ERR_EISDIR;
    }

    output.data = buf;
    output.cap = *in_out_len;
    output.len = 0u;
    if (strcmp(path, "/host/sys/health") == 0) {
        render_health(&output);
    } else if (strcmp(path, "/host/sys/links") == 0) {
        render_links(&output);
    } else if (strcmp(path, "/host/sys/topology") == 0) {
        render_topology(&output);
    } else if (strcmp(path, "/host/sys/routes") == 0) {
        render_routes(&output);
    } else if (strcmp(path, "/host/sys/sessions") == 0) {
        render_sessions(&output);
    } else if (strcmp(path, "/host/sys/web") == 0) {
        render_web(&output);
    } else if (strcmp(path, "/host/sys/log") == 0) {
        render_log(&output);
    } else if (strcmp(path, "/host/sys/jobs") == 0) {
        render_jobs(&output);
    } else if (strcmp(path, "/host/sys/hosts") == 0) {
        render_hosts(&output);
    }
    *in_out_len = (uint16_t)output.len;
    return 0;
}

static void fill_dirent(struct m9p_dirent *entry, const host_node_t *node)
{
    memset(entry, 0, sizeof(*entry));
    entry->qid.type = node->directory != 0u ?
        (uint8_t)(M9P_QID_DIR | M9P_QID_VIRTUAL) :
        (uint8_t)(M9P_QID_VIRTUAL | M9P_QID_READONLY);
    entry->qid.object_id = node->object_id;
    entry->perm = PWOS_HOST_PERM_READ;
    entry->flags = node->directory != 0u ?
        (uint8_t)(M9P_STAT_DIR | M9P_STAT_VIRTUAL) : M9P_STAT_VIRTUAL;
    (void)snprintf(entry->name, sizeof(entry->name), "%s", node->name);
}

int pwos_host_observability_list(
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count)
{
    size_t first;
    size_t count;
    size_t i;

    if (path == NULL || out_count == NULL ||
        (max_entries > 0u && entries == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }
    *out_count = 0u;
    if (strcmp(path, "/host") == 0) {
        first = 1u;
        count = 1u;
    } else if (strcmp(path, "/host/sys") == 0) {
        first = 2u;
        count = sizeof(g_host_nodes) / sizeof(g_host_nodes[0]) - first;
    } else if (find_node(path) != NULL) {
        return -(int)M9P_ERR_ENOTDIR;
    } else {
        return -(int)M9P_ERR_ENOENT;
    }

    if (count > max_entries) {
        count = max_entries;
    }
    for (i = 0u; i < count; ++i) {
        fill_dirent(&entries[i], &g_host_nodes[first + i]);
    }
    *out_count = count;
    return 0;
}

int pwos_host_observability_stat(
    const char *path,
    struct m9p_stat *out_stat)
{
    const host_node_t *node;

    if (path == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }

    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->qid.type = node->directory != 0u ?
        (uint8_t)(M9P_QID_DIR | M9P_QID_VIRTUAL) :
        (uint8_t)(M9P_QID_VIRTUAL | M9P_QID_READONLY);
    out_stat->qid.object_id = node->object_id;
    out_stat->perm = PWOS_HOST_PERM_READ;
    out_stat->flags = node->directory != 0u ?
        (uint8_t)(M9P_STAT_DIR | M9P_STAT_VIRTUAL) : M9P_STAT_VIRTUAL;
    out_stat->mtime = (uint32_t)(esp_timer_get_time() / 1000000);
    (void)snprintf(out_stat->name, sizeof(out_stat->name), "%s", node->name);
    return 0;
}
