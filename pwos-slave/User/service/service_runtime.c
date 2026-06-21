#include "service_runtime.h"

#include <string.h>

#include "local_vfs.h"
#include "mini9p_protocol.h"
#include "mini9p_server.h"
#include "node_control.h"
#include "pwos_link_frame.h"

typedef struct {
    uint8_t initialized;
    struct local_vfs vfs;
    struct m9p_server server;
    uint8_t response_payload[PWOS_LINK_MAX_PAYLOAD_LEN];
    pwos_service_runtime_stats_t stats;
} pwos_service_runtime_t;

static pwos_service_runtime_t g_service;

static int decode_link_view(
    const pwos_frame_block_t *block,
    pwos_link_frame_view_t *out_view)
{
    if (block == NULL || out_view == NULL || block->len == 0u) {
        return -1;
    }

    return pwos_link_decode(block->data, block->len, out_view) == PWOS_OK ? 0 : -1;
}

int pwos_service_runtime_init(void)
{
    struct local_vfs_config vfs_config;
    struct m9p_server_config server_config;

    memset(&g_service, 0, sizeof(g_service));

    local_vfs_get_default_config(&vfs_config);
    if (local_vfs_init(&g_service.vfs, &vfs_config) != 0) {
        return -1;
    }

    m9p_server_get_default_config(&server_config);
    server_config.ops = local_vfs_ops();
    server_config.ops_ctx = &g_service.vfs;
    server_config.max_msize = PWOS_LINK_MAX_PAYLOAD_LEN;
    server_config.default_iounit = LOCAL_VFS_DEFAULT_IOUNIT;
    if (m9p_server_init(&g_service.server, &server_config) != 0) {
        return -1;
    }

    g_service.initialized = 1u;
    g_service.stats.initialized = 1u;
    return 0;
}

int pwos_service_runtime_accepts(const pwos_frame_block_t *block)
{
    pwos_link_frame_view_t view;
    pwos_node_control_snapshot_t node;

    if (g_service.initialized == 0u || decode_link_view(block, &view) != 0) {
        return 0;
    }
    if (view.type != (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P) {
        return 0;
    }

    pwos_node_control_get_snapshot(&node);
    return node.state == PWOS_NODE_ASSIGNED && view.dst == node.local_addr ? 1 : 0;
}

void pwos_service_runtime_process(pwos_frame_block_t *block)
{
    pwos_link_frame_view_t link_view;
    struct m9p_frame_view m9p_view;
    size_t response_len = 0u;
    int rc;

    if (g_service.initialized == 0u || decode_link_view(block, &link_view) != 0) {
        ++g_service.stats.bad_frames;
        return;
    }

    if (link_view.type != (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P) {
        ++g_service.stats.unsupported_frames;
        return;
    }

    ++g_service.stats.mini9p_rx;
    g_service.stats.last_src = link_view.src;
    g_service.stats.last_dst = link_view.dst;
    if (m9p_decode_frame(link_view.payload, link_view.payload_len, &m9p_view)) {
        g_service.stats.last_m9p_type = m9p_view.type;
        g_service.stats.last_m9p_tag = m9p_view.tag;
    }

    /*
     * mini9P server 串行运行在 service_task 内。TWRITE 的 data 指针只在本调用
     * 期间有效，响应写入独立缓冲，避免覆盖请求 payload。
     */
    rc = m9p_server_handle_frame(
        &g_service.server,
        link_view.payload,
        link_view.payload_len,
        g_service.response_payload,
        sizeof(g_service.response_payload),
        &response_len);
    if (rc != 0) {
        ++g_service.stats.server_errors;
    }
    if (response_len == 0u || response_len > PWOS_LINK_MAX_PAYLOAD_LEN) {
        ++g_service.stats.server_errors;
        return;
    }

    if (pwos_node_control_send_data(
            (uint8_t)PWOS_LINK_TYPE_DATA_MINI9P,
            link_view.src,
            g_service.response_payload,
            (uint16_t)response_len) != 0) {
        ++g_service.stats.tx_failures;
        return;
    }

    ++g_service.stats.mini9p_tx;
}

void pwos_service_runtime_get_stats(pwos_service_runtime_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    *out_stats = g_service.stats;
}
