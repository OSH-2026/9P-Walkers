#include "rpc_client.h"

#include <string.h>

#include "pwos_link_frame.h"

static int rpc_retag_adapter(
    uint8_t *frame,
    size_t frame_len,
    uint16_t wire_tag)
{
    /* session_manager 统一分配 wire_tag，RPC 内部字段名叫 call_id。 */
    return pwos_rpc_retag(frame, frame_len, wire_tag);
}

static uint32_t effective_deadline(uint32_t deadline_ms)
{
    return deadline_ms == 0u ? PWOS_RPC_CLIENT_DEFAULT_DEADLINE_MS : deadline_ms;
}

static uint32_t effective_local_deadline(uint32_t remote_deadline)
{
    /*
     * 远端 deadline 写进 RPC 帧，让 slave 知道业务截止时间；
     * 本地多等 100ms grace，用于接收远端赶在 deadline 附近返回的响应。
     */
    return remote_deadline > UINT32_MAX - PWOS_RPC_CLIENT_DEADLINE_GRACE_MS ?
        UINT32_MAX : remote_deadline + PWOS_RPC_CLIENT_DEADLINE_GRACE_MS;
}

static void send_cancel(
    pwos_rpc_client_t *client,
    uint8_t addr,
    uint32_t boot_id,
    uint16_t call_id)
{
    uint8_t frame[PWOS_RPC_HEADER_LEN];
    size_t frame_len = 0u;

    if (call_id == 0u || pwos_rpc_encode(
            PWOS_RPC_KIND_CANCEL,
            0u,
            call_id,
            0u,
            0u,
            NULL,
            NULL,
            NULL,
            0u,
            frame,
            sizeof(frame),
            &frame_len) != 0) {
        return;
    }
    if (pwos_session_manager_send_data(
            client->sessions,
            addr,
            boot_id,
            (uint8_t)PWOS_LINK_TYPE_DATA_RPC,
            frame,
            (uint16_t)frame_len) == 0) {
        ++client->stats.cancel_tx;
    }
}

int pwos_rpc_client_init(
    pwos_rpc_client_t *client,
    pwos_session_manager_t *sessions)
{
    if (client == NULL || sessions == NULL) {
        return -1;
    }
    memset(client, 0, sizeof(*client));
    client->sessions = sessions;
    return 0;
}

int pwos_rpc_client_call(
    pwos_rpc_client_t *client,
    uint8_t addr,
    uint32_t boot_id,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status)
{
    uint8_t request_frame[PWOS_RPC_MAX_FRAME_LEN];
    uint8_t response_frame[PWOS_RPC_MAX_FRAME_LEN];
    pwos_rpc_frame_view_t view;
    size_t request_len = 0u;
    size_t response_len = 0u;
    uint16_t call_id = 0u;
    uint32_t remote_deadline;
    uint32_t local_deadline;
    int rc;

    if (client == NULL || client->sessions == NULL || service == NULL ||
        method == NULL || in_out_response_len == NULL || out_status == NULL ||
        (*in_out_response_len > 0u && response == NULL)) {
        return -1;
    }
    remote_deadline = effective_deadline(deadline_ms);
    local_deadline = effective_local_deadline(remote_deadline);
    /*
     * call_id 先填 0，真正发出前由 session_manager 通过 rpc_retag_adapter()
     * 替换为唯一 wire_tag。
     */
    if (pwos_rpc_encode(
            PWOS_RPC_KIND_REQUEST,
            0u,
            0u,
            0u,
            remote_deadline,
            service,
            method,
            payload,
            payload_len,
            request_frame,
            sizeof(request_frame),
            &request_len) != 0) {
        return -1;
    }

    ++client->stats.unary_tx;
    rc = pwos_session_manager_request_data(
        client->sessions,
        addr,
        boot_id,
        (uint8_t)PWOS_LINK_TYPE_DATA_RPC,
        request_frame,
        request_len,
        rpc_retag_adapter,
        local_deadline,
        response_frame,
        sizeof(response_frame),
        &response_len,
        &call_id);
    client->stats.last_call_id = call_id;
    if (rc != 0) {
        client->stats.last_error = rc;
        if (rc == PWOS_SESSION_ERR_DEADLINE) {
            /* 本地等超时后，尽力发送 CANCEL，远端收到后可中止仍在运行的调用。 */
            ++client->stats.deadline_errors;
            send_cancel(client, addr, boot_id, call_id);
        }
        return rc;
    }
    if (pwos_rpc_decode(response_frame, response_len, &view) != 0 ||
        (view.kind != PWOS_RPC_KIND_RESPONSE &&
         view.kind != PWOS_RPC_KIND_STREAM_END) ||
        view.call_id != call_id || view.payload_len > *in_out_response_len) {
        /* 响应类型、call_id、长度任一不匹配，都按 malformed 处理。 */
        ++client->stats.malformed_responses;
        client->stats.last_error = -1;
        return -1;
    }

    if (view.payload_len > 0u) {
        memcpy(response, view.payload, view.payload_len);
    }
    *in_out_response_len = view.payload_len;
    *out_status = view.status;
    client->stats.last_status = view.status;
    client->stats.last_error = 0;
    ++client->stats.unary_rx;
    if (view.status != PWOS_RPC_STATUS_OK) {
        ++client->stats.remote_errors;
    }
    return 0;
}

int pwos_rpc_client_stream(
    pwos_rpc_client_t *client,
    uint8_t addr,
    uint32_t boot_id,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response,
    uint16_t *in_out_response_len,
    uint16_t *out_status,
    uint16_t *out_chunk_count)
{
    uint8_t request_frame[PWOS_RPC_MAX_FRAME_LEN];
    size_t request_len = 0u;
    size_t response_len = 0u;
    uint16_t call_id = 0u;
    uint16_t remote_status = PWOS_RPC_STATUS_OK;
    uint16_t chunk_count = 0u;
    uint32_t remote_deadline;
    int rc;

    if (client == NULL || client->sessions == NULL || service == NULL ||
        method == NULL || in_out_response_len == NULL || out_status == NULL ||
        out_chunk_count == NULL ||
        (*in_out_response_len > 0u && response == NULL)) {
        return -1;
    }
    remote_deadline = effective_deadline(deadline_ms);
    /* STREAM flag 告诉 slave 返回 STREAM_CHUNK* + STREAM_END，而不是单个 RESPONSE。 */
    if (pwos_rpc_encode(
            PWOS_RPC_KIND_REQUEST,
            PWOS_RPC_FLAG_STREAM,
            0u,
            0u,
            remote_deadline,
            service,
            method,
            payload,
            payload_len,
            request_frame,
            sizeof(request_frame),
            &request_len) != 0) {
        return -1;
    }

    ++client->stats.stream_tx;
    /*
     * chunk 顺序和聚合不在 rpc_client 做，而是在 session_manager_deliver_data_part()
     * 中按 status_or_part_index 校验连续性。
     */
    rc = pwos_session_manager_request_stream_data(
        client->sessions,
        addr,
        boot_id,
        (uint8_t)PWOS_LINK_TYPE_DATA_RPC,
        request_frame,
        request_len,
        rpc_retag_adapter,
        effective_local_deadline(remote_deadline),
        response,
        *in_out_response_len,
        &response_len,
        &call_id,
        &remote_status,
        &chunk_count);
    client->stats.last_call_id = call_id;
    if (rc != 0) {
        client->stats.last_error = rc;
        if (rc == PWOS_SESSION_ERR_DEADLINE) {
            /* 流式调用超时同样发送 CANCEL，call_id 即本次 stream 的 wire_tag。 */
            ++client->stats.deadline_errors;
            send_cancel(client, addr, boot_id, call_id);
        }
        return rc;
    }

    *in_out_response_len = (uint16_t)response_len;
    *out_status = remote_status;
    *out_chunk_count = chunk_count;
    client->stats.last_status = remote_status;
    client->stats.last_error = 0;
    ++client->stats.stream_rx;
    client->stats.stream_chunks_rx += chunk_count;
    if (remote_status != PWOS_RPC_STATUS_OK) {
        ++client->stats.remote_errors;
    }
    return 0;
}

int pwos_rpc_client_notify(
    pwos_rpc_client_t *client,
    uint8_t addr,
    uint32_t boot_id,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms)
{
    uint8_t frame[PWOS_RPC_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint16_t call_id = 0u;
    int rc;

    if (client == NULL || client->sessions == NULL ||
        pwos_rpc_encode(
            PWOS_RPC_KIND_REQUEST,
            PWOS_RPC_FLAG_ONEWAY,
            0u,
            0u,
            effective_deadline(deadline_ms),
            service,
            method,
            payload,
            payload_len,
            frame,
            sizeof(frame),
            &frame_len) != 0) {
        return -1;
    }
    /*
     * notify 是 one-way：分配 call_id 并发送，但不占 pending、不等待响应。
     */
    rc = pwos_session_manager_send_oneway_data(
        client->sessions,
        addr,
        boot_id,
        (uint8_t)PWOS_LINK_TYPE_DATA_RPC,
        frame,
        frame_len,
        rpc_retag_adapter,
        &call_id);
    client->stats.last_call_id = call_id;
    client->stats.last_error = rc;
    if (rc == 0) {
        ++client->stats.oneway_tx;
    }
    return rc;
}

void pwos_rpc_client_get_stats(
    const pwos_rpc_client_t *client,
    pwos_rpc_client_stats_t *out_stats)
{
    if (client == NULL || out_stats == NULL) {
        return;
    }
    *out_stats = client->stats;
}
