#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pwos_rpc_protocol.h"

static void test_request_round_trip(void)
{
    static const uint8_t payload[] = {1u, 2u, 3u, 4u};
    uint8_t frame[PWOS_RPC_MAX_FRAME_LEN];
    pwos_rpc_frame_view_t view;
    size_t frame_len = 0u;

    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_REQUEST,
        0u,
        7u,
        0u,
        250u,
        "system",
        "ping",
        payload,
        sizeof(payload),
        frame,
        sizeof(frame),
        &frame_len) == 0);
    assert(pwos_rpc_decode(frame, frame_len, &view) == 0);
    assert(view.kind == PWOS_RPC_KIND_REQUEST);
    assert(view.call_id == 7u);
    assert(view.deadline_ms == 250u);
    assert(pwos_rpc_name_equals(view.service, view.service_len, "system"));
    assert(pwos_rpc_name_equals(view.method, view.method_len, "ping"));
    assert(view.payload_len == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);

    assert(pwos_rpc_retag(frame, frame_len, 99u) == 0);
    assert(pwos_rpc_decode(frame, frame_len, &view) == 0);
    assert(view.call_id == 99u);
}

static void test_response_and_cancel(void)
{
    uint8_t frame[64];
    pwos_rpc_frame_view_t view;
    size_t frame_len = 0u;

    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_RESPONSE, 0u, 42u, PWOS_RPC_STATUS_DEADLINE,
        0u, NULL, NULL, NULL, 0u,
        frame, sizeof(frame), &frame_len) == 0);
    assert(pwos_rpc_decode(frame, frame_len, &view) == 0);
    assert(view.call_id == 42u);
    assert(view.status == PWOS_RPC_STATUS_DEADLINE);

    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_CANCEL, 0u, 42u, 0u, 0u,
        NULL, NULL, NULL, 0u,
        frame, sizeof(frame), &frame_len) == 0);
    assert(pwos_rpc_decode(frame, frame_len, &view) == 0);
    assert(view.kind == PWOS_RPC_KIND_CANCEL);
}

static void test_rejects_malformed_frames(void)
{
    uint8_t frame[64];
    pwos_rpc_frame_view_t view;
    size_t frame_len = 0u;

    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_REQUEST, 0u, 1u, 0u, 10u,
        "s", "m", NULL, 0u,
        frame, sizeof(frame), &frame_len) == 0);
    assert(pwos_rpc_decode(frame, frame_len - 1u, &view) != 0);
    frame[0] = 0xFFu;
    assert(pwos_rpc_decode(frame, frame_len, &view) != 0);
    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_REQUEST, 0u, 1u, 0u, 10u,
        "", "m", NULL, 0u,
        frame, sizeof(frame), &frame_len) != 0);
}

static void test_stream_frames(void)
{
    uint8_t frame[64];
    pwos_rpc_frame_view_t view;
    size_t frame_len = 0u;

    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_STREAM_CHUNK,
        0u,
        55u,
        2u,
        0u,
        NULL,
        NULL,
        (const uint8_t *)"chunk",
        5u,
        frame,
        sizeof(frame),
        &frame_len) == 0);
    assert(pwos_rpc_decode(frame, frame_len, &view) == 0);
    assert(view.kind == PWOS_RPC_KIND_STREAM_CHUNK);
    assert(view.call_id == 55u);
    assert(view.status == 2u);
    assert(view.payload_len == 5u);

    assert(pwos_rpc_encode(
        PWOS_RPC_KIND_STREAM_END,
        0u,
        55u,
        PWOS_RPC_STATUS_OK,
        0u,
        NULL,
        NULL,
        NULL,
        0u,
        frame,
        sizeof(frame),
        &frame_len) == 0);
    assert(pwos_rpc_decode(frame, frame_len, &view) == 0);
    assert(view.kind == PWOS_RPC_KIND_STREAM_END);
}

int main(void)
{
    test_request_round_trip();
    test_response_and_cancel();
    test_rejects_malformed_frames();
    test_stream_frames();
    puts("pwos rpc protocol tests passed");
    return 0;
}
