#include "rpc_service.h"

#include <stdio.h>
#include <string.h>

static uint32_t service_now(pwos_rpc_service_t *service)
{
    return service->now_ms == NULL ? 0u : service->now_ms(service->io_ctx);
}

static int time_reached(uint32_t now, uint32_t target)
{
    return (int32_t)(now - target) >= 0;
}

static uint32_t pending_count(const pwos_rpc_service_t *service)
{
    size_t i;
    uint32_t count = 0u;

    for (i = 0u; i < PWOS_RPC_SERVICE_MAX_PENDING; ++i) {
        if (service->pending[i].used != 0u) {
            ++count;
        }
    }
    return count;
}

static pwos_rpc_pending_call_t *find_pending(
    pwos_rpc_service_t *service,
    uint8_t src_addr,
    uint16_t call_id)
{
    size_t i;

    for (i = 0u; i < PWOS_RPC_SERVICE_MAX_PENDING; ++i) {
        if (service->pending[i].used != 0u &&
            service->pending[i].src_addr == src_addr &&
            service->pending[i].call_id == call_id) {
            return &service->pending[i];
        }
    }
    return NULL;
}

static pwos_rpc_pending_call_t *alloc_pending(pwos_rpc_service_t *service)
{
    size_t i;

    for (i = 0u; i < PWOS_RPC_SERVICE_MAX_PENDING; ++i) {
        if (service->pending[i].used == 0u) {
            return &service->pending[i];
        }
    }
    return NULL;
}

static const pwos_rpc_method_t *find_method(
    const pwos_rpc_service_t *service,
    const pwos_rpc_frame_view_t *request)
{
    size_t i;

    for (i = 0u; i < service->method_count; ++i) {
        if (pwos_rpc_name_equals(
                request->service, request->service_len, service->methods[i].service) &&
            pwos_rpc_name_equals(
                request->method, request->method_len, service->methods[i].method)) {
            return &service->methods[i];
        }
    }
    return NULL;
}

static int send_frame(
    pwos_rpc_service_t *service,
    uint8_t dst_addr,
    uint8_t kind,
    uint16_t call_id,
    uint16_t status,
    const uint8_t *payload,
    uint16_t payload_len)
{
    size_t frame_len = 0u;
    int rc;

    if (pwos_rpc_encode(
            kind,
            0u,
            call_id,
            status,
            0u,
            NULL,
            NULL,
            payload,
            payload_len,
            service->tx_frame,
            sizeof(service->tx_frame),
            &frame_len) != 0) {
        ++service->stats.send_failures;
        return -1;
    }
    rc = service->send(
        service->io_ctx, dst_addr, service->tx_frame, (uint16_t)frame_len);
    if (rc != 0) {
        ++service->stats.send_failures;
        return rc;
    }
    if (kind == PWOS_RPC_KIND_RESPONSE) {
        ++service->stats.response_tx;
    } else if (kind == PWOS_RPC_KIND_STREAM_CHUNK) {
        ++service->stats.stream_chunk_tx;
    } else if (kind == PWOS_RPC_KIND_STREAM_END) {
        ++service->stats.stream_end_tx;
    }
    service->stats.last_call_id = call_id;
    service->stats.last_status = status;
    return 0;
}

static int send_response(
    pwos_rpc_service_t *service,
    uint8_t dst_addr,
    uint16_t call_id,
    uint16_t status,
    const uint8_t *payload,
    uint16_t payload_len)
{
    return send_frame(
        service, dst_addr, PWOS_RPC_KIND_RESPONSE, call_id,
        status, payload, payload_len);
}

static int send_terminal(
    pwos_rpc_service_t *service,
    uint8_t dst_addr,
    const pwos_rpc_frame_view_t *request,
    uint16_t status)
{
    return send_frame(
        service,
        dst_addr,
        (request->flags & PWOS_RPC_FLAG_STREAM) != 0u ?
            PWOS_RPC_KIND_STREAM_END : PWOS_RPC_KIND_RESPONSE,
        request->call_id,
        status,
        NULL,
        0u);
}

static int process_cancel(
    pwos_rpc_service_t *service,
    uint8_t src_addr,
    const pwos_rpc_frame_view_t *frame)
{
    pwos_rpc_pending_call_t *pending;

    ++service->stats.cancel_rx;
    pending = find_pending(service, src_addr, frame->call_id);
    if (pending != NULL) {
        memset(pending, 0, sizeof(*pending));
    }
    /* CANCEL 是单向控制，不发送无人等待的确认响应。 */
    return 0;
}

static int queue_deferred(
    pwos_rpc_service_t *service,
    uint8_t src_addr,
    const pwos_rpc_frame_view_t *frame,
    uint16_t status,
    const pwos_rpc_result_t *result)
{
    pwos_rpc_pending_call_t *pending;
    uint32_t count;
    uint32_t now = service_now(service);
    uint8_t streaming =
        (frame->flags & PWOS_RPC_FLAG_STREAM) != 0u ? 1u : 0u;

    if (find_pending(service, src_addr, frame->call_id) != NULL ||
        result->payload_len > PWOS_RPC_SERVICE_DEFERRED_PAYLOAD_CAP) {
        ++service->stats.busy;
        return send_terminal(service, src_addr, frame, PWOS_RPC_STATUS_BUSY);
    }
    pending = alloc_pending(service);
    if (pending == NULL) {
        ++service->stats.busy;
        return send_terminal(service, src_addr, frame, PWOS_RPC_STATUS_BUSY);
    }

    memset(pending, 0, sizeof(*pending));
    pending->used = 1u;
    pending->streaming = streaming;
    pending->src_addr = src_addr;
    pending->call_id = frame->call_id;
    pending->status = status;
    pending->due_ms = now + result->defer_ms;
    pending->has_deadline = frame->deadline_ms != 0u ? 1u : 0u;
    pending->deadline_at_ms = now + frame->deadline_ms;
    pending->payload_len = result->payload_len;
    if (result->payload_len > 0u) {
        memcpy(pending->payload, result->payload, result->payload_len);
    }
    count = pending_count(service);
    if (count > service->stats.pending_peak) {
        service->stats.pending_peak = count;
    }
    return 0;
}

int pwos_rpc_service_init(
    pwos_rpc_service_t *service,
    void *io_ctx,
    pwos_rpc_service_send_fn send,
    pwos_rpc_service_now_fn now_ms)
{
    if (service == NULL || send == NULL || now_ms == NULL) {
        return -1;
    }
    memset(service, 0, sizeof(*service));
    service->io_ctx = io_ctx;
    service->send = send;
    service->now_ms = now_ms;
    return 0;
}

int pwos_rpc_service_register(
    pwos_rpc_service_t *service,
    const char *service_name,
    const char *method_name,
    pwos_rpc_method_fn handler,
    void *handler_ctx)
{
    pwos_rpc_method_t *entry;

    if (service == NULL || service_name == NULL || method_name == NULL ||
        handler == NULL || strlen(service_name) == 0u || strlen(method_name) == 0u ||
        strlen(service_name) > PWOS_RPC_MAX_NAME_LEN ||
        strlen(method_name) > PWOS_RPC_MAX_NAME_LEN ||
        service->method_count >= PWOS_RPC_SERVICE_MAX_METHODS) {
        return -1;
    }
    entry = &service->methods[service->method_count++];
    entry->service = service_name;
    entry->method = method_name;
    entry->handler = handler;
    entry->handler_ctx = handler_ctx;
    return 0;
}

int pwos_rpc_service_process(
    pwos_rpc_service_t *service,
    uint8_t src_addr,
    const uint8_t *frame,
    uint16_t frame_len)
{
    pwos_rpc_frame_view_t view;
    const pwos_rpc_method_t *method;
    pwos_rpc_request_t request;
    pwos_rpc_result_t result;
    uint16_t status;

    if (service == NULL || pwos_rpc_decode(frame, frame_len, &view) != 0 ||
        view.call_id == 0u) {
        if (service != NULL) {
            ++service->stats.bad_frames;
        }
        return -1;
    }
    if (view.kind == PWOS_RPC_KIND_CANCEL) {
        return process_cancel(service, src_addr, &view);
    }
    if (view.kind != PWOS_RPC_KIND_REQUEST) {
        ++service->stats.bad_frames;
        return -1;
    }

    ++service->stats.request_rx;
    service->stats.last_call_id = view.call_id;
    if ((view.flags & PWOS_RPC_FLAG_STREAM) != 0u) {
        ++service->stats.stream_request_rx;
    }
    if ((view.flags & (PWOS_RPC_FLAG_ONEWAY | PWOS_RPC_FLAG_STREAM)) ==
        (PWOS_RPC_FLAG_ONEWAY | PWOS_RPC_FLAG_STREAM)) {
        ++service->stats.bad_frames;
        return -1;
    }
    if ((view.flags & PWOS_RPC_FLAG_ONEWAY) != 0u) {
        ++service->stats.oneway_rx;
    }
    method = find_method(service, &view);
    if (method == NULL) {
        ++service->stats.not_found;
        if ((view.flags & PWOS_RPC_FLAG_ONEWAY) != 0u) {
            return 0;
        }
        return send_terminal(service, src_addr, &view, PWOS_RPC_STATUS_NOT_FOUND);
    }

    memset(&request, 0, sizeof(request));
    request.payload = view.payload;
    request.payload_len = view.payload_len;
    request.src_addr = src_addr;
    request.call_id = view.call_id;
    request.deadline_ms = view.deadline_ms;
    request.flags = view.flags;
    memset(&result, 0, sizeof(result));
    result.payload = service->handler_output;
    result.payload_cap = sizeof(service->handler_output);
    status = method->handler(method->handler_ctx, &request, &result);
    if (result.payload_len > result.payload_cap) {
        status = PWOS_RPC_STATUS_INTERNAL;
        result.payload_len = 0u;
    }

    if ((view.flags & PWOS_RPC_FLAG_ONEWAY) != 0u) {
        ++service->stats.completed;
        return 0;
    }
    if (result.defer_ms > 0u ||
        (view.flags & PWOS_RPC_FLAG_STREAM) != 0u) {
        return queue_deferred(service, src_addr, &view, status, &result);
    }
    ++service->stats.completed;
    return send_response(
        service, src_addr, view.call_id, status, result.payload, result.payload_len);
}

void pwos_rpc_service_poll(pwos_rpc_service_t *service)
{
    uint32_t now;
    size_t i;

    if (service == NULL) {
        return;
    }
    now = service_now(service);
    for (i = 0u; i < PWOS_RPC_SERVICE_MAX_PENDING; ++i) {
        pwos_rpc_pending_call_t *pending = &service->pending[i];
        uint8_t deadline_reached;
        int rc;

        if (pending->used == 0u) {
            continue;
        }
        deadline_reached = pending->has_deadline != 0u &&
            time_reached(now, pending->deadline_at_ms) ? 1u : 0u;
        if (deadline_reached == 0u && !time_reached(now, pending->due_ms)) {
            continue;
        }

        if (deadline_reached != 0u) {
            rc = send_frame(
                service,
                pending->src_addr,
                pending->streaming != 0u ?
                    PWOS_RPC_KIND_STREAM_END : PWOS_RPC_KIND_RESPONSE,
                pending->call_id,
                PWOS_RPC_STATUS_DEADLINE,
                NULL,
                0u);
            ++service->stats.deadline_tx;
            (void)rc;
            memset(pending, 0, sizeof(*pending));
            continue;
        }

        if (pending->streaming != 0u) {
            if (pending->stream_offset < pending->payload_len) {
                uint16_t remaining =
                    (uint16_t)(pending->payload_len - pending->stream_offset);
                uint16_t chunk_len = remaining > PWOS_RPC_SERVICE_STREAM_CHUNK_CAP ?
                    PWOS_RPC_SERVICE_STREAM_CHUNK_CAP : remaining;

                rc = send_frame(
                    service,
                    pending->src_addr,
                    PWOS_RPC_KIND_STREAM_CHUNK,
                    pending->call_id,
                    (uint16_t)(pending->stream_offset /
                        PWOS_RPC_SERVICE_STREAM_CHUNK_CAP),
                    pending->payload + pending->stream_offset,
                    chunk_len);
                if (rc != 0) {
                    memset(pending, 0, sizeof(*pending));
                    continue;
                }
                pending->stream_offset =
                    (uint16_t)(pending->stream_offset + chunk_len);
                pending->due_ms = now + PWOS_RPC_SERVICE_STREAM_INTERVAL_MS;
                continue;
            }
            rc = send_frame(
                service,
                pending->src_addr,
                PWOS_RPC_KIND_STREAM_END,
                pending->call_id,
                pending->status,
                NULL,
                0u);
            if (rc == 0) {
                ++service->stats.completed;
            }
            memset(pending, 0, sizeof(*pending));
            continue;
        }

        rc = send_response(
            service,
            pending->src_addr,
            pending->call_id,
            pending->status,
            pending->payload,
            pending->payload_len);
        if (rc == 0) {
            ++service->stats.completed;
        }
        memset(pending, 0, sizeof(*pending));
    }
}

void pwos_rpc_service_get_stats(
    const pwos_rpc_service_t *service,
    pwos_rpc_service_stats_t *out_stats)
{
    if (service == NULL || out_stats == NULL) {
        return;
    }
    *out_stats = service->stats;
}

static uint16_t ping_handler(
    void *ctx,
    const pwos_rpc_request_t *request,
    pwos_rpc_result_t *result)
{
    (void)ctx;
    if (request->payload_len > result->payload_cap) {
        return PWOS_RPC_STATUS_BAD_REQUEST;
    }
    memcpy(result->payload, request->payload, request->payload_len);
    result->payload_len = request->payload_len;
    return PWOS_RPC_STATUS_OK;
}

static uint16_t info_handler(
    void *ctx,
    const pwos_rpc_request_t *request,
    pwos_rpc_result_t *result)
{
    static const char info[] = "pwos-node rpc-v1";

    (void)ctx;
    (void)request;
    memcpy(result->payload, info, sizeof(info) - 1u);
    result->payload_len = sizeof(info) - 1u;
    return PWOS_RPC_STATUS_OK;
}

static uint16_t notify_handler(
    void *ctx,
    const pwos_rpc_request_t *request,
    pwos_rpc_result_t *result)
{
    pwos_rpc_service_t *service = (pwos_rpc_service_t *)ctx;

    (void)request;
    (void)result;
    ++service->stats.notify_count;
    return PWOS_RPC_STATUS_OK;
}

static uint16_t delay_handler(
    void *ctx,
    const pwos_rpc_request_t *request,
    pwos_rpc_result_t *result)
{
    char text[16];
    unsigned long delay_ms;
    char trailing;
    int parsed;

    (void)ctx;
    if (request->payload_len == 0u || request->payload_len >= sizeof(text)) {
        return PWOS_RPC_STATUS_BAD_REQUEST;
    }
    memcpy(text, request->payload, request->payload_len);
    text[request->payload_len] = '\0';
    parsed = sscanf(text, "%lu%c", &delay_ms, &trailing);
    if (parsed != 1 || delay_ms == 0u || delay_ms > 60000u) {
        return PWOS_RPC_STATUS_BAD_REQUEST;
    }
    memcpy(result->payload, "done", 4u);
    result->payload_len = 4u;
    result->defer_ms = (uint32_t)delay_ms;
    return PWOS_RPC_STATUS_OK;
}

static uint16_t fail_handler(
    void *ctx,
    const pwos_rpc_request_t *request,
    pwos_rpc_result_t *result)
{
    (void)ctx;
    (void)request;
    (void)result;
    return PWOS_RPC_STATUS_INTERNAL;
}

int pwos_rpc_service_register_builtins(pwos_rpc_service_t *service)
{
    int rc = 0;

    rc |= pwos_rpc_service_register(service, "system", "ping", ping_handler, service);
    rc |= pwos_rpc_service_register(service, "system", "stream", ping_handler, service);
    rc |= pwos_rpc_service_register(service, "system", "info", info_handler, service);
    rc |= pwos_rpc_service_register(service, "system", "notify", notify_handler, service);
    rc |= pwos_rpc_service_register(service, "system", "delay", delay_handler, service);
    rc |= pwos_rpc_service_register(service, "system", "fail", fail_handler, service);
    return rc == 0 ? 0 : -1;
}
