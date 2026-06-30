#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rpc_service.h"

typedef struct {
    uint32_t now_ms;
    uint32_t send_count;
    uint8_t last_dst;
    uint8_t frame[PWOS_RPC_MAX_FRAME_LEN];
    uint16_t frame_len;
} fake_io_t;

static uint32_t fake_now(void *ctx)
{
    return ((fake_io_t *)ctx)->now_ms;
}

static int fake_send(
    void *ctx,
    uint8_t dst_addr,
    const uint8_t *payload,
    uint16_t payload_len)
{
    fake_io_t *io = (fake_io_t *)ctx;

    assert(payload_len <= sizeof(io->frame));
    io->last_dst = dst_addr;
    io->frame_len = payload_len;
    memcpy(io->frame, payload, payload_len);
    ++io->send_count;
    return 0;
}

static void init_service(pwos_rpc_service_t *service, fake_io_t *io)
{
    memset(io, 0, sizeof(*io));
    assert(pwos_rpc_service_init(service, io, fake_send, fake_now) == 0);
    assert(pwos_rpc_service_register_builtins(service) == 0);
}

static size_t build_request(
    uint8_t *frame,
    uint8_t flags,
    uint16_t call_id,
    uint32_t deadline_ms,
    const char *method,
    const char *payload)
{
    size_t frame_len = 0u;
    uint16_t payload_len = payload == NULL ? 0u : (uint16_t)strlen(payload);

    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_REQUEST,
        flags,
        call_id,
        0u,
        deadline_ms,
        "system",
        method,
        (const uint8_t *)payload,
        payload_len,
        frame,
        PWOS_RPC_MAX_FRAME_LEN,
        &frame_len) == 0);
    return frame_len;
}

static pwos_rpc_frame_view_t decode_last(const fake_io_t *io)
{
    pwos_rpc_frame_view_t view;

    assert(io->frame_len > 0u);
    assert(pwos_rpc_decode(io->frame, io->frame_len, &view) == 0);
    return view;
}

static void test_ping_and_not_found(void)
{
    pwos_rpc_service_t service;
    fake_io_t io;
    uint8_t request[PWOS_RPC_MAX_FRAME_LEN];
    size_t request_len;
    pwos_rpc_frame_view_t response;

    init_service(&service, &io);
    request_len = build_request(request, 0u, 7u, 100u, "ping", "abc");
    assert(pwos_rpc_service_process(&service, 2u, request, request_len) == 0);
    response = decode_last(&io);
    assert(io.last_dst == 2u);
    assert(response.call_id == 7u);
    assert(response.status == PWOS_RPC_STATUS_OK);
    assert(response.payload_len == 3u);
    assert(memcmp(response.payload, "abc", 3u) == 0);

    request_len = build_request(request, 0u, 8u, 100u, "missing", NULL);
    assert(pwos_rpc_service_process(&service, 2u, request, request_len) == 0);
    response = decode_last(&io);
    assert(response.status == PWOS_RPC_STATUS_NOT_FOUND);
}

static void test_oneway_notify(void)
{
    pwos_rpc_service_t service;
    pwos_rpc_service_stats_t stats;
    fake_io_t io;
    uint8_t request[PWOS_RPC_MAX_FRAME_LEN];
    size_t request_len;

    init_service(&service, &io);
    request_len = build_request(
        request, PWOS_RPC_FLAG_ONEWAY, 9u, 100u, "notify", "event");
    assert(pwos_rpc_service_process(&service, 1u, request, request_len) == 0);
    assert(io.send_count == 0u);
    pwos_rpc_service_get_stats(&service, &stats);
    assert(stats.oneway_rx == 1u);
    assert(stats.notify_count == 1u);
}

static void test_deferred_completion_and_deadline(void)
{
    pwos_rpc_service_t service;
    fake_io_t io;
    uint8_t request[PWOS_RPC_MAX_FRAME_LEN];
    size_t request_len;
    pwos_rpc_frame_view_t response;

    init_service(&service, &io);
    io.now_ms = 1000u;
    request_len = build_request(request, 0u, 10u, 0u, "delay", "25");
    assert(pwos_rpc_service_process(&service, 1u, request, request_len) == 0);
    assert(io.send_count == 0u);
    io.now_ms = 1025u;
    pwos_rpc_service_poll(&service);
    response = decode_last(&io);
    assert(response.status == PWOS_RPC_STATUS_OK);
    assert(response.payload_len == 4u);
    assert(memcmp(response.payload, "done", 4u) == 0);

    request_len = build_request(request, 0u, 11u, 20u, "delay", "50");
    assert(pwos_rpc_service_process(&service, 1u, request, request_len) == 0);
    io.now_ms = 1045u;
    pwos_rpc_service_poll(&service);
    response = decode_last(&io);
    assert(response.call_id == 11u);
    assert(response.status == PWOS_RPC_STATUS_DEADLINE);
}

static void test_cancel_removes_deferred_call(void)
{
    pwos_rpc_service_t service;
    pwos_rpc_service_stats_t stats;
    fake_io_t io;
    uint8_t request[PWOS_RPC_MAX_FRAME_LEN];
    size_t request_len;
    size_t cancel_len = 0u;

    init_service(&service, &io);
    request_len = build_request(request, 0u, 12u, 1000u, "delay", "100");
    assert(pwos_rpc_service_process(&service, 3u, request, request_len) == 0);
    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_CANCEL, 0u, 12u, 0u, 0u,
        NULL, NULL, NULL, 0u,
        request, sizeof(request), &cancel_len) == 0);
    assert(pwos_rpc_service_process(&service, 3u, request, cancel_len) == 0);
    io.now_ms = 200u;
    pwos_rpc_service_poll(&service);
    assert(io.send_count == 0u);
    pwos_rpc_service_get_stats(&service, &stats);
    assert(stats.cancel_rx == 1u);
}

static void test_stream_chunks_and_terminal(void)
{
    static const char payload[] =
        "0123456789012345678901234567890123456789012345678901234567890123456789";
    pwos_rpc_service_t service;
    pwos_rpc_service_stats_t stats;
    fake_io_t io;
    uint8_t request[PWOS_RPC_MAX_FRAME_LEN];
    size_t request_len;
    pwos_rpc_frame_view_t frame;

    init_service(&service, &io);
    request_len = build_request(
        request, PWOS_RPC_FLAG_STREAM, 13u, 200u, "stream", payload);
    assert(pwos_rpc_service_process(&service, 2u, request, request_len) == 0);
    assert(io.send_count == 0u);

    pwos_rpc_service_poll(&service);
    frame = decode_last(&io);
    assert(frame.kind == PWOS_RPC_KIND_STREAM_CHUNK);
    assert(frame.status == 0u);
    assert(frame.payload_len == PWOS_RPC_SERVICE_STREAM_CHUNK_CAP);
    assert(memcmp(frame.payload, payload, frame.payload_len) == 0);

    io.now_ms = 9u;
    pwos_rpc_service_poll(&service);
    assert(io.send_count == 1u);
    io.now_ms = 10u;
    pwos_rpc_service_poll(&service);
    frame = decode_last(&io);
    assert(frame.kind == PWOS_RPC_KIND_STREAM_CHUNK);
    assert(frame.status == 1u);
    assert(memcmp(
        frame.payload,
        payload + PWOS_RPC_SERVICE_STREAM_CHUNK_CAP,
        frame.payload_len) == 0);

    io.now_ms = 20u;
    pwos_rpc_service_poll(&service);
    frame = decode_last(&io);
    assert(frame.kind == PWOS_RPC_KIND_STREAM_CHUNK);
    assert(frame.status == 2u);
    assert(frame.payload_len == sizeof(payload) - 1u -
        2u * PWOS_RPC_SERVICE_STREAM_CHUNK_CAP);
    io.now_ms = 30u;
    pwos_rpc_service_poll(&service);
    frame = decode_last(&io);
    assert(frame.kind == PWOS_RPC_KIND_STREAM_END);
    assert(frame.status == PWOS_RPC_STATUS_OK);

    pwos_rpc_service_get_stats(&service, &stats);
    assert(stats.stream_request_rx == 1u);
    assert(stats.stream_chunk_tx == 3u);
    assert(stats.stream_end_tx == 1u);
    assert(stats.completed == 1u);
}

static void test_stream_deadline_uses_terminal_frame(void)
{
    pwos_rpc_service_t service;
    fake_io_t io;
    uint8_t request[PWOS_RPC_MAX_FRAME_LEN];
    size_t request_len;
    pwos_rpc_frame_view_t frame;

    init_service(&service, &io);
    request_len = build_request(
        request, PWOS_RPC_FLAG_STREAM, 14u, 20u, "delay", "100");
    assert(pwos_rpc_service_process(&service, 2u, request, request_len) == 0);
    io.now_ms = 20u;
    pwos_rpc_service_poll(&service);
    frame = decode_last(&io);
    assert(frame.kind == PWOS_RPC_KIND_STREAM_END);
    assert(frame.call_id == 14u);
    assert(frame.status == PWOS_RPC_STATUS_DEADLINE);
    assert(frame.payload_len == 0u);
}

int main(void)
{
    test_ping_and_not_found();
    test_oneway_notify();
    test_deferred_completion_and_deadline();
    test_cancel_removes_deferred_call();
    test_stream_chunks_and_terminal();
    test_stream_deadline_uses_terminal_frame();
    puts("pwos rpc service tests passed");
    return 0;
}
