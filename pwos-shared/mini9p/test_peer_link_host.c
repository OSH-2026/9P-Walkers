#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mini9p_peer_link.h"

struct fake_frame {
    uint8_t data[128];
    size_t len;
};

struct fake_link_io {
    struct fake_frame rx_frames[8];
    size_t rx_count;
    size_t rx_index;
    struct fake_frame tx_frames[8];
    size_t tx_count;
    size_t foreign_count;
    uint16_t last_foreign_tag;
};

static void fake_link_reset(struct fake_link_io *io)
{
    memset(io, 0, sizeof(*io));
}

static void fake_link_queue_rx(struct fake_link_io *io, const uint8_t *data, size_t len)
{
    assert(io->rx_count < (sizeof(io->rx_frames) / sizeof(io->rx_frames[0])));
    assert(len <= sizeof(io->rx_frames[io->rx_count].data));
    memcpy(io->rx_frames[io->rx_count].data, data, len);
    io->rx_frames[io->rx_count].len = len;
    ++io->rx_count;
}

static int fake_send_frame(void *transport_ctx, const uint8_t *tx_data, size_t tx_len)
{
    struct fake_link_io *io = (struct fake_link_io *)transport_ctx;

    assert(io->tx_count < (sizeof(io->tx_frames) / sizeof(io->tx_frames[0])));
    assert(tx_len <= sizeof(io->tx_frames[io->tx_count].data));
    memcpy(io->tx_frames[io->tx_count].data, tx_data, tx_len);
    io->tx_frames[io->tx_count].len = tx_len;
    ++io->tx_count;
    return 0;
}

static int fake_receive_frame(void *transport_ctx, uint8_t *rx_data, size_t rx_cap, size_t *rx_len)
{
    struct fake_link_io *io = (struct fake_link_io *)transport_ctx;

    if (io->rx_index >= io->rx_count) {
        return -(int)M9P_ERR_EAGAIN;
    }
    if (io->rx_frames[io->rx_index].len > rx_cap) {
        return -(int)M9P_ERR_EMSIZE;
    }

    memcpy(rx_data, io->rx_frames[io->rx_index].data, io->rx_frames[io->rx_index].len);
    *rx_len = io->rx_frames[io->rx_index].len;
    ++io->rx_index;
    return 0;
}

static int fake_request_handler(
    void *handler_ctx,
    const uint8_t *request_data,
    size_t request_len,
    uint8_t *response_data,
    size_t response_cap,
    size_t *response_len)
{
    struct fake_link_io *io = (struct fake_link_io *)handler_ctx;
    struct m9p_frame_view frame;

    assert(m9p_decode_frame(request_data, request_len, &frame));
    io->last_foreign_tag = frame.tag;
    return m9p_build_rclunk(frame.tag, response_data, response_cap, response_len)
               ? 0
               : -(int)M9P_ERR_EMSIZE;
}

static int fake_foreign_frame_handler(
    void *handler_ctx,
    const uint8_t *frame_data,
    size_t frame_len,
    const struct m9p_frame_view *frame)
{
    struct fake_link_io *io = (struct fake_link_io *)handler_ctx;

    (void)frame_data;
    (void)frame_len;
    ++io->foreign_count;
    io->last_foreign_tag = frame->tag;
    return 0;
}

static void init_link(
    struct m9p_peer_link *link,
    struct fake_link_io *io,
    uint8_t *dispatch_rx,
    size_t dispatch_rx_cap,
    uint8_t *dispatch_tx,
    size_t dispatch_tx_cap)
{
    struct m9p_peer_link_config config;

    m9p_peer_link_get_default_config(&config);
    config.send_frame = fake_send_frame;
    config.receive_frame = fake_receive_frame;
    config.transport_ctx = io;
    config.request_handler = fake_request_handler;
    config.request_handler_ctx = io;
    config.dispatch_rx_buffer = dispatch_rx;
    config.dispatch_rx_cap = dispatch_rx_cap;
    config.dispatch_tx_buffer = dispatch_tx;
    config.dispatch_tx_cap = dispatch_tx_cap;
    assert(m9p_peer_link_init(link, &config) == 0);
}

static void test_request_returns_matching_response(void)
{
    struct fake_link_io io;
    struct m9p_peer_link link;
    uint8_t dispatch_rx[128];
    uint8_t dispatch_tx[128];
    uint8_t request[64];
    uint8_t response[64];
    uint8_t actual[64];
    size_t request_len;
    size_t response_len;
    size_t actual_len;

    fake_link_reset(&io);
    init_link(&link, &io, dispatch_rx, sizeof(dispatch_rx), dispatch_tx, sizeof(dispatch_tx));
    assert(m9p_build_tclunk(0x1111u, 3u, request, sizeof(request), &request_len));
    assert(m9p_build_rclunk(0x1111u, response, sizeof(response), &response_len));
    fake_link_queue_rx(&io, response, response_len);

    assert(m9p_peer_link_request(&link, request, request_len, actual, sizeof(actual), &actual_len) == 0);
    assert(io.tx_count == 1u);
    assert(io.tx_frames[0].len == request_len);
    assert(memcmp(io.tx_frames[0].data, request, request_len) == 0);
    assert(actual_len == response_len);
    assert(memcmp(actual, response, response_len) == 0);
}

static void test_request_services_foreign_request_while_waiting(void)
{
    struct fake_link_io io;
    struct m9p_peer_link link;
    uint8_t dispatch_rx[128];
    uint8_t dispatch_tx[128];
    uint8_t local_request[64];
    uint8_t foreign_request[64];
    uint8_t local_response[64];
    uint8_t actual[64];
    size_t local_request_len;
    size_t foreign_request_len;
    size_t local_response_len;
    size_t actual_len;

    fake_link_reset(&io);
    init_link(&link, &io, dispatch_rx, sizeof(dispatch_rx), dispatch_tx, sizeof(dispatch_tx));
    assert(m9p_build_tclunk(0x2001u, 7u, local_request, sizeof(local_request), &local_request_len));
    assert(m9p_build_tclunk(0x3002u, 9u, foreign_request, sizeof(foreign_request), &foreign_request_len));
    assert(m9p_build_rclunk(0x2001u, local_response, sizeof(local_response), &local_response_len));
    fake_link_queue_rx(&io, foreign_request, foreign_request_len);
    fake_link_queue_rx(&io, local_response, local_response_len);

    assert(m9p_peer_link_request(&link,
                                 local_request,
                                 local_request_len,
                                 actual,
                                 sizeof(actual),
                                 &actual_len) == 0);
    assert(io.tx_count == 2u);
    assert(memcmp(io.tx_frames[0].data, local_request, local_request_len) == 0);
    assert(io.last_foreign_tag == 0x3002u);
    assert(actual_len == local_response_len);
    assert(memcmp(actual, local_response, local_response_len) == 0);
}

static void test_request_skips_unmatched_response(void)
{
    struct fake_link_io io;
    struct m9p_peer_link link;
    uint8_t dispatch_rx[128];
    uint8_t dispatch_tx[128];
    uint8_t request[64];
    uint8_t stray_response[64];
    uint8_t wanted_response[64];
    uint8_t actual[64];
    size_t request_len;
    size_t stray_len;
    size_t wanted_len;
    size_t actual_len;

    fake_link_reset(&io);
    init_link(&link, &io, dispatch_rx, sizeof(dispatch_rx), dispatch_tx, sizeof(dispatch_tx));
    link.config.foreign_frame_handler = fake_foreign_frame_handler;
    link.config.foreign_frame_handler_ctx = &io;
    assert(m9p_build_tclunk(0x4444u, 5u, request, sizeof(request), &request_len));
    assert(m9p_build_rclunk(0x9999u, stray_response, sizeof(stray_response), &stray_len));
    assert(m9p_build_rclunk(0x4444u, wanted_response, sizeof(wanted_response), &wanted_len));
    fake_link_queue_rx(&io, stray_response, stray_len);
    fake_link_queue_rx(&io, wanted_response, wanted_len);

    assert(m9p_peer_link_request(&link, request, request_len, actual, sizeof(actual), &actual_len) == 0);
    assert(io.foreign_count == 1u);
    assert(io.last_foreign_tag == 0x9999u);
    assert(actual_len == wanted_len);
    assert(memcmp(actual, wanted_response, wanted_len) == 0);
}

static void test_poll_once_handles_foreign_request(void)
{
    struct fake_link_io io;
    struct m9p_peer_link link;
    uint8_t dispatch_rx[128];
    uint8_t dispatch_tx[128];
    uint8_t foreign_request[64];
    uint8_t expected_response[64];
    size_t foreign_request_len;
    size_t expected_response_len;

    fake_link_reset(&io);
    init_link(&link, &io, dispatch_rx, sizeof(dispatch_rx), dispatch_tx, sizeof(dispatch_tx));
    assert(m9p_build_tclunk(0x5151u, 12u, foreign_request, sizeof(foreign_request), &foreign_request_len));
    assert(m9p_build_rclunk(0x5151u, expected_response, sizeof(expected_response), &expected_response_len));
    fake_link_queue_rx(&io, foreign_request, foreign_request_len);

    assert(m9p_peer_link_poll_once(&link) == 0);
    assert(io.tx_count == 1u);
    assert(io.tx_frames[0].len == expected_response_len);
    assert(memcmp(io.tx_frames[0].data, expected_response, expected_response_len) == 0);
}

int main(void)
{
    test_request_returns_matching_response();
    test_request_services_foreign_request_while_waiting();
    test_request_skips_unmatched_response();
    test_poll_once_handles_foreign_request();
    puts("peer_link host tests passed");
    return 0;
}