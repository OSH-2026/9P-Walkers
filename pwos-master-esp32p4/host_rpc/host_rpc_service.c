#include "host_rpc_service.h"

#include <stdio.h>
#include <string.h>

#include "mini9p_protocol.h"
#include "session_manager.h"

static int field_equals(const char *field, uint8_t field_len, const char *expected)
{
    size_t expected_len = strlen(expected);

    return field_len == expected_len && memcmp(field, expected, expected_len) == 0;
}

static uint16_t map_local_error(int rc)
{
    if (rc == 0) return PWOS_HOST_RPC_STATUS_OK;
    if (rc == PWOS_SESSION_ERR_DEADLINE) return PWOS_HOST_RPC_STATUS_DEADLINE;
    if (rc == PWOS_SESSION_ERR_NO_ROUTE || rc == PWOS_SESSION_ERR_STALE_BOOT) {
        return PWOS_HOST_RPC_STATUS_NO_ROUTE;
    }
    if (rc == PWOS_SESSION_ERR_QUEUE_FULL || rc == -(int)M9P_ERR_EBUSY ||
        rc == -(int)M9P_ERR_EAGAIN) return PWOS_HOST_RPC_STATUS_BUSY;
    if (rc == -(int)M9P_ERR_ENOENT) return PWOS_HOST_RPC_STATUS_NOT_FOUND;
    if (rc == -(int)M9P_ERR_EINVAL || rc == -(int)M9P_ERR_EOFFS ||
        rc == -(int)M9P_ERR_EMSIZE) return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    return PWOS_HOST_RPC_STATUS_INTERNAL;
}

static int copy_field(
    const char *source,
    uint8_t source_len,
    char *out,
    size_t out_cap)
{
    if (source == NULL || source_len == 0u || source_len >= out_cap) {
        return -1;
    }
    memcpy(out, source, source_len);
    out[source_len] = '\0';
    return 0;
}

static uint16_t dispatch_read(
    pwos_host_rpc_service_t *service,
    const pwos_host_rpc_frame_view_t *request,
    uint16_t *out_payload_len)
{
    pwos_host_rpc_read_node_view_t args;
    char target[PWOS_HOST_RPC_TARGET_CAP];
    char path[PWOS_HOST_RPC_PATH_CAP];
    uint16_t data_len;
    int rc;

    ++service->stats.read_calls;
    if (service->config.read_node == NULL ||
        pwos_host_rpc_decode_read_node(request->payload, request->payload_len, &args) != 0 ||
        copy_field(args.target, args.target_len, target, sizeof(target)) != 0 ||
        copy_field(args.path, args.path_len, path, sizeof(path)) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }
    data_len = args.max_bytes;
    if (data_len > sizeof(service->data)) {
        data_len = sizeof(service->data);
    }
    rc = service->config.read_node(
        service->config.ctx,
        target,
        path,
        service->data,
        &data_len,
        request->deadline_ms);
    if (rc != 0) {
        return map_local_error(rc);
    }
    if (pwos_host_rpc_encode_blob(
            service->data,
            data_len,
            service->method_payload,
            sizeof(service->method_payload),
            out_payload_len) != 0) {
        return PWOS_HOST_RPC_STATUS_INTERNAL;
    }
    return PWOS_HOST_RPC_STATUS_OK;
}

static uint16_t dispatch_write(
    pwos_host_rpc_service_t *service,
    const pwos_host_rpc_frame_view_t *request,
    uint16_t *out_payload_len)
{
    pwos_host_rpc_write_node_view_t args;
    char target[PWOS_HOST_RPC_TARGET_CAP];
    char path[PWOS_HOST_RPC_PATH_CAP];
    uint8_t written_bytes[2];
    uint16_t written = 0u;
    int rc;

    ++service->stats.write_calls;
    if (service->config.write_node == NULL ||
        pwos_host_rpc_decode_write_node(request->payload, request->payload_len, &args) != 0 ||
        copy_field(args.target, args.target_len, target, sizeof(target)) != 0 ||
        copy_field(args.path, args.path_len, path, sizeof(path)) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }
    rc = service->config.write_node(
        service->config.ctx,
        target,
        path,
        args.data,
        args.data_len,
        &written,
        request->deadline_ms);
    if (rc != 0) {
        return map_local_error(rc);
    }
    written_bytes[0] = (uint8_t)(written >> 8);
    written_bytes[1] = (uint8_t)written;
    if (pwos_host_rpc_encode_blob(
            written_bytes,
            sizeof(written_bytes),
            service->method_payload,
            sizeof(service->method_payload),
            out_payload_len) != 0) {
        return PWOS_HOST_RPC_STATUS_INTERNAL;
    }
    return PWOS_HOST_RPC_STATUS_OK;
}

static uint16_t dispatch_advertise(
    pwos_host_rpc_service_t *service,
    const pwos_host_rpc_frame_view_t *request,
    uint16_t *out_payload_len)
{
    pwos_host_rpc_advertise_t advertise;
    pwos_host_rpc_advertise_t local;
    int rc;

    ++service->stats.advertise_calls;
    if (service->config.advertise == NULL || service->config.local_advertise == NULL ||
        pwos_host_rpc_decode_advertise(
            request->payload, request->payload_len, &advertise) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }
    rc = service->config.advertise(service->config.ctx, &advertise);
    if (rc != 0) {
        return map_local_error(rc);
    }
    memset(&local, 0, sizeof(local));
    rc = service->config.local_advertise(service->config.ctx, &local);
    if (rc != 0) {
        return map_local_error(rc);
    }
    return pwos_host_rpc_encode_advertise(
        &local, service->method_payload,
        sizeof(service->method_payload), out_payload_len) == 0 ?
        PWOS_HOST_RPC_STATUS_OK : PWOS_HOST_RPC_STATUS_INTERNAL;
}

static uint16_t dispatch_whoowns(
    pwos_host_rpc_service_t *service,
    const pwos_host_rpc_frame_view_t *request,
    uint16_t *out_payload_len)
{
    pwos_host_rpc_advertise_t owner;
    const char *target_view;
    char target[PWOS_HOST_RPC_TARGET_CAP];
    uint8_t target_len;
    int rc;

    ++service->stats.whoowns_calls;
    if (service->config.whoowns == NULL ||
        pwos_host_rpc_decode_text(
            request->payload, request->payload_len,
            &target_view, &target_len) != 0 ||
        copy_field(target_view, target_len, target, sizeof(target)) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }
    memset(&owner, 0, sizeof(owner));
    rc = service->config.whoowns(service->config.ctx, target, &owner);
    if (rc != 0) {
        return map_local_error(rc);
    }
    return pwos_host_rpc_encode_advertise(
        &owner,
        service->method_payload,
        sizeof(service->method_payload),
        out_payload_len) == 0 ?
        PWOS_HOST_RPC_STATUS_OK : PWOS_HOST_RPC_STATUS_INTERNAL;
}

static uint16_t dispatch_topology_sync(
    pwos_host_rpc_service_t *service,
    const pwos_host_rpc_frame_view_t *request,
    uint16_t *out_payload_len)
{
    int rc;

    ++service->stats.topology_sync_calls;
    if (service->config.topology_sync == NULL ||
        pwos_host_rpc_decode_topology(
            request->payload,
            request->payload_len,
            &service->incoming_topology) != 0) {
        return PWOS_HOST_RPC_STATUS_BAD_REQUEST;
    }
    memset(&service->outgoing_topology, 0, sizeof(service->outgoing_topology));
    rc = service->config.topology_sync(
        service->config.ctx,
        &service->incoming_topology,
        &service->outgoing_topology);
    if (rc != 0) {
        return map_local_error(rc);
    }
    return pwos_host_rpc_encode_topology(
        &service->outgoing_topology,
        service->method_payload,
        sizeof(service->method_payload),
        out_payload_len) == 0 ?
        PWOS_HOST_RPC_STATUS_OK : PWOS_HOST_RPC_STATUS_INTERNAL;
}

int pwos_host_rpc_service_init(
    pwos_host_rpc_service_t *service,
    const pwos_host_rpc_service_config_t *config)
{
    if (service == NULL || config == NULL) {
        return -1;
    }
    memset(service, 0, sizeof(*service));
    service->config = *config;
    return 0;
}

int pwos_host_rpc_service_handle(
    pwos_host_rpc_service_t *service,
    const uint8_t *request_frame,
    size_t request_len,
    uint8_t *response_frame,
    size_t response_cap,
    size_t *out_response_len)
{
    pwos_host_rpc_frame_view_t request;
    const char *response_service = "host";
    const char *response_method = "error";
    uint16_t method_payload_len = 0u;
    uint16_t status = PWOS_HOST_RPC_STATUS_NOT_FOUND;

    if (service == NULL || response_frame == NULL || out_response_len == NULL ||
        pwos_host_rpc_decode(request_frame, request_len, &request) != 0 ||
        request.kind != PWOS_HOST_RPC_KIND_REQUEST) {
        if (service != NULL) {
            ++service->stats.bad_frames;
        }
        return -1;
    }
    ++service->stats.requests;
    service->stats.last_call_id = request.call_id;
    if (field_equals(request.service, request.service_len, "cluster") &&
        field_equals(request.method, request.method_len, "read_node")) {
        response_service = "cluster";
        response_method = "read_node";
        status = dispatch_read(service, &request, &method_payload_len);
    } else if (field_equals(request.service, request.service_len, "cluster") &&
               field_equals(request.method, request.method_len, "write_node")) {
        response_service = "cluster";
        response_method = "write_node";
        status = dispatch_write(service, &request, &method_payload_len);
    } else if (field_equals(request.service, request.service_len, "host") &&
               field_equals(request.method, request.method_len, "advertise")) {
        response_service = "host";
        response_method = "advertise";
        status = dispatch_advertise(service, &request, &method_payload_len);
    } else if (field_equals(request.service, request.service_len, "topology") &&
               field_equals(request.method, request.method_len, "whoowns")) {
        response_service = "topology";
        response_method = "whoowns";
        status = dispatch_whoowns(service, &request, &method_payload_len);
    } else if (field_equals(request.service, request.service_len, "topology") &&
               field_equals(request.method, request.method_len, "sync")) {
        response_service = "topology";
        response_method = "sync";
        status = dispatch_topology_sync(service, &request, &method_payload_len);
    } else {
        ++service->stats.not_found;
    }
    if (status != PWOS_HOST_RPC_STATUS_OK) {
        method_payload_len = 0u;
        ++service->stats.remote_errors;
    }
    if (pwos_host_rpc_encode(
            PWOS_HOST_RPC_KIND_RESPONSE,
            request.call_id,
            0u,
            status,
            response_service,
            response_method,
            method_payload_len == 0u ? NULL : service->method_payload,
            method_payload_len,
            response_frame,
            response_cap,
            out_response_len) != 0) {
        return -1;
    }
    ++service->stats.responses;
    service->stats.last_status = status;
    return 0;
}

void pwos_host_rpc_service_get_stats(
    const pwos_host_rpc_service_t *service,
    pwos_host_rpc_service_stats_t *out_stats)
{
    if (service != NULL && out_stats != NULL) {
        *out_stats = service->stats;
    }
}
