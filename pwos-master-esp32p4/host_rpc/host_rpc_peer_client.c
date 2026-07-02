/* ========================================================================
 * Host RPC Peer 客户端 —— 实现
 *
 * 与远端 Host 建立 TCP 连接，发送编码后的 RPC 请求帧，解析响应。
 *
 * 主要函数:
 *   call()        — 通用 RPC 调用 (编码 → exchange → 解码)
 *   read_node()   — 封装 cluster.read_node
 *   write_node()  — 封装 cluster.write_node
 * ======================================================================== */

#include "host_rpc_peer_client.h"

#include <string.h>

#include "mini9p_protocol.h"

/* ========================================================================
 * 第一节: 线程安全锁
 * ======================================================================== */

static void client_lock(pwos_host_rpc_peer_client_t *client)
{
    if (client->config.lock)
        client->config.lock(client->config.lock_ctx);
}

static void client_unlock(pwos_host_rpc_peer_client_t *client)
{
    if (client->config.unlock)
        client->config.unlock(client->config.lock_ctx);
}

/* ========================================================================
 * 第二节: 初始化
 * ======================================================================== */

int pwos_host_rpc_peer_client_init(
    pwos_host_rpc_peer_client_t              *client,
    const pwos_host_rpc_peer_client_config_t *config)
{
    /* 参数校验: exchange 必选; lock/unlock 必须成对提供或都为 NULL */
    if (!client || !config || !config->exchange ||
        (!config->lock != !config->unlock)) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(client, 0, sizeof(*client));
    client->config = *config;  /* 浅拷贝配置 */
    return 0;
}

/* ========================================================================
 * 第三节: 通用 RPC 调用
 *
 * 流程:
 *   1. 分配 call_id (自增, 跳过 0)
 *   2. 编码请求帧 (service + method + payload)
 *   3. 通过 exchange 回调发送并接收响应
 *   4. 解码响应帧 → 校验 call_id 匹配 → 拷贝 payload → 输出 status
 * ======================================================================== */

int pwos_host_rpc_peer_client_call(
    pwos_host_rpc_peer_client_t *client,
    const char *service, const char *method,
    const uint8_t *payload, uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response_payload, uint16_t *in_out_response_len,
    uint16_t *out_status)
{
    /* ---- 参数校验 ---- */
    if (!client || !service || !method ||
        !in_out_response_len || !out_status ||
        (*in_out_response_len > 0u && !response_payload)) {
        return -(int)M9P_ERR_EINVAL;
    }

    /* ---- 分配 call_id (线程安全) ---- */
    client_lock(client);
    client->next_call_id++;
    if (client->next_call_id == 0u) client->next_call_id++;  /* 跳过 0 */
    uint32_t call_id = client->next_call_id;
    client->stats.calls++;
    client->stats.last_call_id = call_id;
    client_unlock(client);

    /* ---- 编码请求帧 ---- */
    uint8_t request[PWOS_HOST_RPC_MAX_FRAME_LEN];
    size_t  request_len = 0u;

    if (pwos_host_rpc_encode(PWOS_HOST_RPC_KIND_REQUEST, call_id, deadline_ms,
                             PWOS_HOST_RPC_STATUS_OK, service, method,
                             payload, payload_len,
                             request, sizeof(request), &request_len) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }

    /* ---- TCP 帧交换 ---- */
    uint8_t response[PWOS_HOST_RPC_MAX_FRAME_LEN];
    size_t  response_len = 0u;

    int rc = client->config.exchange(client->config.io_ctx,
                                     request, request_len,
                                     response, sizeof(response), &response_len,
                                     deadline_ms);
    client_lock(client);
    client->stats.last_error = rc;
    if (rc != 0) {
        client->stats.transport_errors++;
        client_unlock(client);
        return rc;
    }
    client_unlock(client);

    /* ---- 解码响应帧 ---- */
    pwos_host_rpc_frame_view_t view;
    if (pwos_host_rpc_decode(response, response_len, &view) != 0 ||
        view.kind != PWOS_HOST_RPC_KIND_RESPONSE || view.call_id != call_id) {
        client_lock(client);
        client->stats.malformed_responses++;
        client->stats.last_error = -(int)M9P_ERR_EIO;
        client_unlock(client);
        return -(int)M9P_ERR_EIO;
    }

    /* ---- 拷贝响应 payload ---- */
    if (view.payload_len > *in_out_response_len)
        return -(int)M9P_ERR_EMSIZE;

    if (view.payload_len > 0u)
        memcpy(response_payload, view.payload, view.payload_len);
    *in_out_response_len = view.payload_len;
    *out_status           = view.status;

    /* ---- 更新统计 ---- */
    client_lock(client);
    client->stats.responses++;
    client->stats.last_status = view.status;
    client->stats.last_error  = 0;
    if (view.status != PWOS_HOST_RPC_STATUS_OK)
        client->stats.remote_errors++;
    client_unlock(client);

    return 0;
}

/* ========================================================================
 * 第四节: 节点文件读写 (封装 cluster.read_node / cluster.write_node)
 * ======================================================================== */

int pwos_host_rpc_peer_client_read_node(
    pwos_host_rpc_peer_client_t *client,
    const char *target, const char *path,
    uint8_t *data, uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    if (!client || !in_out_len || (*in_out_len > 0u && !data))
        return -(int)M9P_ERR_EINVAL;

    /* 编码 read_node 参数 */
    uint8_t  args[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint16_t args_len = 0u;
    if (pwos_host_rpc_encode_read_node(target, path, *in_out_len,
                                       args, sizeof(args), &args_len) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }

    /* 发起 RPC 调用 */
    uint8_t  result[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint16_t result_len = sizeof(result);
    uint16_t status;
    int rc = pwos_host_rpc_peer_client_call(
        client, "cluster", "read_node",
        args, args_len, deadline_ms,
        result, &result_len, &status);

    if (rc != 0) return rc;
    if (status != PWOS_HOST_RPC_STATUS_OK)
        return -(PWOS_HOST_RPC_REMOTE_ERROR_BASE + (int)status);

    /* 解码 blob 响应 */
    const uint8_t *blob;
    uint16_t blob_len = 0u;
    if (pwos_host_rpc_decode_blob(result, result_len, &blob, &blob_len) != 0 ||
        blob_len > *in_out_len) {
        return -(int)M9P_ERR_EIO;
    }

    if (blob_len > 0u) memcpy(data, blob, blob_len);
    *in_out_len = blob_len;
    return 0;
}

int pwos_host_rpc_peer_client_write_node(
    pwos_host_rpc_peer_client_t *client,
    const char *target, const char *path,
    const uint8_t *data, uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    if (!client || !out_written)
        return -(int)M9P_ERR_EINVAL;

    /* 编码 write_node 参数 */
    uint8_t  args[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint16_t args_len = 0u;
    if (pwos_host_rpc_encode_write_node(target, path, data, data_len,
                                        args, sizeof(args), &args_len) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }

    /* 发起 RPC 调用 */
    uint8_t  result[16];
    uint16_t result_len = sizeof(result);
    uint16_t status;
    int rc = pwos_host_rpc_peer_client_call(
        client, "cluster", "write_node",
        args, args_len, deadline_ms,
        result, &result_len, &status);

    if (rc != 0) return rc;
    if (status != PWOS_HOST_RPC_STATUS_OK)
        return -(PWOS_HOST_RPC_REMOTE_ERROR_BASE + (int)status);

    /* 解码写入字节数 (blob 中 2 字节大端) */
    const uint8_t *blob;
    uint16_t blob_len = 0u;
    if (pwos_host_rpc_decode_blob(result, result_len, &blob, &blob_len) != 0 ||
        blob_len != 2u) {
        return -(int)M9P_ERR_EIO;
    }
    *out_written = (uint16_t)(((uint16_t)blob[0] << 8) | blob[1]);
    return 0;
}

/* ========================================================================
 * 第五节: 统计查询
 * ======================================================================== */

void pwos_host_rpc_peer_client_get_stats(
    pwos_host_rpc_peer_client_t       *client,
    pwos_host_rpc_peer_client_stats_t *out_stats)
{
    if (!client || !out_stats) return;
    client_lock(client);
    *out_stats = client->stats;
    client_unlock(client);
}
