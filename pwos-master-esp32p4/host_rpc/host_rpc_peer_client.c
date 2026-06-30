#include "host_rpc_peer_client.h"

#include <string.h>

#include "mini9p_protocol.h"

static void client_lock(pwos_host_rpc_peer_client_t *client)
{
    if (client->config.lock != NULL) client->config.lock(client->config.lock_ctx);
}

static void client_unlock(pwos_host_rpc_peer_client_t *client)
{
    if (client->config.unlock != NULL) client->config.unlock(client->config.lock_ctx);
}

int pwos_host_rpc_peer_client_init(
    pwos_host_rpc_peer_client_t *client,
    const pwos_host_rpc_peer_client_config_t *config)
{
    if (client == NULL || config == NULL || config->exchange == NULL ||
        (config->lock == NULL) != (config->unlock == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }
    memset(client, 0, sizeof(*client));
    client->config = *config;
    return 0;
}

int pwos_host_rpc_peer_client_call(
    pwos_host_rpc_peer_client_t *client,
    const char *service,
    const char *method,
    const uint8_t *payload,
    uint16_t payload_len,
    uint32_t deadline_ms,
    uint8_t *response_payload,
    uint16_t *in_out_response_len,
    uint16_t *out_status)
{
    uint8_t request[PWOS_HOST_RPC_MAX_FRAME_LEN];
    uint8_t response[PWOS_HOST_RPC_MAX_FRAME_LEN];
    pwos_host_rpc_frame_view_t view;
    size_t request_len = 0u;
    size_t response_len = 0u;
    uint32_t call_id;
    int rc;

    if (client == NULL || service == NULL || method == NULL ||
        in_out_response_len == NULL || out_status == NULL ||
        (*in_out_response_len > 0u && response_payload == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }
    client_lock(client);
    ++client->next_call_id;
    if (client->next_call_id == 0u) ++client->next_call_id;
    call_id = client->next_call_id;
    ++client->stats.calls;
    client->stats.last_call_id = call_id;
    client_unlock(client);

    if (pwos_host_rpc_encode(
            PWOS_HOST_RPC_KIND_REQUEST, call_id, deadline_ms,
            PWOS_HOST_RPC_STATUS_OK, service, method,
            payload, payload_len, request, sizeof(request), &request_len) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }
    rc = client->config.exchange(
        client->config.io_ctx, request, request_len,
        response, sizeof(response), &response_len, deadline_ms);
    client_lock(client);
    client->stats.last_error = rc;
    if (rc != 0) {
        ++client->stats.transport_errors;
        client_unlock(client);
        return rc;
    }
    client_unlock(client);
    if (pwos_host_rpc_decode(response, response_len, &view) != 0 ||
        view.kind != PWOS_HOST_RPC_KIND_RESPONSE || view.call_id != call_id) {
        client_lock(client);
        ++client->stats.malformed_responses;
        client->stats.last_error = -(int)M9P_ERR_EIO;
        client_unlock(client);
        return -(int)M9P_ERR_EIO;
    }
    if (view.payload_len > *in_out_response_len) {
        return -(int)M9P_ERR_EMSIZE;
    }
    if (view.payload_len > 0u) {
        memcpy(response_payload, view.payload, view.payload_len);
    }
    *in_out_response_len = view.payload_len;
    *out_status = view.status;
    client_lock(client);
    ++client->stats.responses;
    client->stats.last_status = view.status;
    client->stats.last_error = 0;
    if (view.status != PWOS_HOST_RPC_STATUS_OK) ++client->stats.remote_errors;
    client_unlock(client);
    return 0;
}

int pwos_host_rpc_peer_client_read_node(
    pwos_host_rpc_peer_client_t *client,
    const char *target,
    const char *path,
    uint8_t *data,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    uint8_t args[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint8_t result[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    const uint8_t *blob;
    uint16_t args_len = 0u;
    uint16_t result_len = sizeof(result);
    uint16_t blob_len = 0u;
    uint16_t status;
    int rc;

    if (client == NULL || in_out_len == NULL || (*in_out_len > 0u && data == NULL) ||
        pwos_host_rpc_encode_read_node(
            target, path, *in_out_len, args, sizeof(args), &args_len) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }
    rc = pwos_host_rpc_peer_client_call(
        client, "cluster", "read_node", args, args_len, deadline_ms,
        result, &result_len, &status);
    if (rc != 0) return rc;
    if (status != PWOS_HOST_RPC_STATUS_OK) {
        return -(PWOS_HOST_RPC_REMOTE_ERROR_BASE + (int)status);
    }
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
    const char *target,
    const char *path,
    const uint8_t *data,
    uint16_t data_len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    uint8_t args[PWOS_HOST_RPC_MAX_PAYLOAD_LEN];
    uint8_t result[16];
    const uint8_t *blob;
    uint16_t args_len = 0u;
    uint16_t result_len = sizeof(result);
    uint16_t blob_len = 0u;
    uint16_t status;
    int rc;

    if (client == NULL || out_written == NULL ||
        pwos_host_rpc_encode_write_node(
            target, path, data, data_len, args, sizeof(args), &args_len) != 0) {
        return -(int)M9P_ERR_EINVAL;
    }
    rc = pwos_host_rpc_peer_client_call(
        client, "cluster", "write_node", args, args_len, deadline_ms,
        result, &result_len, &status);
    if (rc != 0) return rc;
    if (status != PWOS_HOST_RPC_STATUS_OK) {
        return -(PWOS_HOST_RPC_REMOTE_ERROR_BASE + (int)status);
    }
    if (pwos_host_rpc_decode_blob(result, result_len, &blob, &blob_len) != 0 ||
        blob_len != 2u) {
        return -(int)M9P_ERR_EIO;
    }
    *out_written = (uint16_t)(((uint16_t)blob[0] << 8) | blob[1]);
    return 0;
}

void pwos_host_rpc_peer_client_get_stats(
    pwos_host_rpc_peer_client_t *client,
    pwos_host_rpc_peer_client_stats_t *out_stats)
{
    if (client == NULL || out_stats == NULL) return;
    client_lock(client);
    *out_stats = client->stats;
    client_unlock(client);
}
