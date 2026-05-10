#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mini9p_protocol.h"
#include "uart_transport.h"
#include "usart.h"

struct fake_uart {
    uint8_t rx_buffer[512];
    size_t rx_len;
    size_t rx_pos;
    uint8_t tx_buffer[512];
    size_t tx_len;
    int flush_count;
    void (*on_transmit)(const uint8_t *data, size_t len);
};

UART_HandleTypeDef huart2;

static struct fake_uart g_uart;
static uint8_t g_scripted_frame[128];
static size_t g_scripted_len;

static struct fake_uart *handle_uart(UART_HandleTypeDef *huart)
{
    return (struct fake_uart *)huart->test_state;
}

static void fake_uart_reset(void)
{
    memset(&g_uart, 0, sizeof(g_uart));
    memset(g_scripted_frame, 0, sizeof(g_scripted_frame));
    g_scripted_len = 0u;
    huart2.test_state = &g_uart;
}

static void fake_uart_append_rx(const uint8_t *data, size_t len)
{
    assert(g_uart.rx_len + len <= sizeof(g_uart.rx_buffer));
    memcpy(g_uart.rx_buffer + g_uart.rx_len, data, len);
    g_uart.rx_len += len;
}

static uint16_t read_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static void append_scripted_response(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    fake_uart_append_rx(g_scripted_frame, g_scripted_len);
}

static int server_handler(void *server_ctx,
                          const uint8_t *request_data,
                          size_t request_len,
                          uint8_t *response_data,
                          size_t response_cap,
                          size_t *response_len)
{
    uint8_t payload[] = {0x44u, 0x66u};
    uint16_t tag;

    (void)server_ctx;
    assert(request_len >= 8u);
    tag = read_le16(request_data + 6);
    assert(m9p_encode_frame(M9P_RWRITE,
                            tag,
                            payload,
                            (uint16_t)sizeof(payload),
                            response_data,
                            response_cap,
                            response_len));
    return 0;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart,
                                    const uint8_t *pData,
                                    uint16_t Size,
                                    uint32_t Timeout)
{
    struct fake_uart *uart = handle_uart(huart);

    (void)Timeout;
    assert(Size <= sizeof(uart->tx_buffer));
    memcpy(uart->tx_buffer, pData, Size);
    uart->tx_len = Size;
    if (uart->on_transmit != NULL) {
        uart->on_transmit(pData, Size);
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *huart,
                                   uint8_t *pData,
                                   uint16_t Size,
                                   uint32_t Timeout)
{
    struct fake_uart *uart = handle_uart(huart);
    size_t available;

    (void)Timeout;
    if (uart->rx_pos >= uart->rx_len) {
        return HAL_TIMEOUT;
    }

    available = uart->rx_len - uart->rx_pos;
    if (available < (size_t)Size) {
        return HAL_TIMEOUT;
    }

    memcpy(pData, uart->rx_buffer + uart->rx_pos, Size);
    uart->rx_pos += Size;
    return HAL_OK;
}

void fake_uart_flush(UART_HandleTypeDef *huart)
{
    struct fake_uart *uart = handle_uart(huart);

    uart->rx_len = 0u;
    uart->rx_pos = 0u;
    uart->flush_count += 1;
}

static void test_send_frame(void)
{
    struct m9p_uart_transport transport;
    struct m9p_uart_transport_config config;
    uint8_t frame[32];
    size_t frame_len;

    fake_uart_reset();
    m9p_uart_transport_get_default_config(&config);
    assert(m9p_uart_transport_init(&transport, &config) == 0);
    assert(m9p_encode_frame(M9P_TSTAT, 0x111u, NULL, 0u, frame, sizeof(frame), &frame_len));
    assert(m9p_uart_transport_send_frame(&transport, frame, frame_len) == 0);
    assert(g_uart.tx_len == frame_len);
    assert(memcmp(g_uart.tx_buffer, frame, frame_len) == 0);
}

static void test_receive_frame(void)
{
    struct m9p_uart_transport transport;
    struct m9p_uart_transport_config config;
    uint8_t expected[64];
    uint8_t actual[64];
    uint8_t payload[] = {0x21u, 0x31u};
    size_t expected_len;
    size_t actual_len;

    fake_uart_reset();
    m9p_uart_transport_get_default_config(&config);
    config.flush_before_receive = false;
    assert(m9p_uart_transport_init(&transport, &config) == 0);
    assert(m9p_encode_frame(M9P_RREAD,
                            0x222u,
                            payload,
                            (uint16_t)sizeof(payload),
                            expected,
                            sizeof(expected),
                            &expected_len));
    fake_uart_append_rx(expected, expected_len);
    assert(m9p_uart_transport_receive_frame(&transport, actual, sizeof(actual), &actual_len) == 0);
    assert(actual_len == expected_len);
    assert(memcmp(actual, expected, expected_len) == 0);
}

static void test_request_round_trip(void)
{
    struct m9p_uart_transport transport;
    struct m9p_uart_transport_config config;
    uint8_t request[64];
    uint8_t response[64];
    uint8_t actual[64];
    uint8_t payload[] = {0x77u, 0x88u};
    size_t request_len;
    size_t response_len;
    size_t actual_len;

    fake_uart_reset();
    m9p_uart_transport_get_default_config(&config);
    config.flush_before_request = false;
    assert(m9p_uart_transport_init(&transport, &config) == 0);
    assert(m9p_encode_frame(M9P_TREAD,
                            0x333u,
                            payload,
                            (uint16_t)sizeof(payload),
                            request,
                            sizeof(request),
                            &request_len));
    assert(m9p_encode_frame(M9P_RREAD,
                            0x333u,
                            payload,
                            (uint16_t)sizeof(payload),
                            response,
                            sizeof(response),
                            &response_len));
    memcpy(g_scripted_frame, response, response_len);
    g_scripted_len = response_len;
    g_uart.on_transmit = append_scripted_response;

    assert(m9p_uart_transport_request(&transport,
                                      request,
                                      request_len,
                                      actual,
                                      sizeof(actual),
                                      &actual_len) == 0);
    assert(actual_len == response_len);
    assert(memcmp(actual, response, response_len) == 0);
}

static void test_direction_flags_are_independent(void)
{
    struct m9p_uart_transport transport;
    struct m9p_uart_transport_config config;
    uint8_t frame[64];
    uint8_t actual[64];
    uint8_t payload[] = {0x19u, 0x91u};
    size_t frame_len;
    size_t actual_len;

    fake_uart_reset();
    m9p_uart_transport_get_default_config(&config);
    config.flush_before_receive = false;
    assert(m9p_uart_transport_init(&transport, &config) == 0);

    transport.tx_busy = true;
    assert(m9p_encode_frame(M9P_RREAD,
                            0x555u,
                            payload,
                            (uint16_t)sizeof(payload),
                            frame,
                            sizeof(frame),
                            &frame_len));
    fake_uart_append_rx(frame, frame_len);
    assert(m9p_uart_transport_receive_frame(&transport, actual, sizeof(actual), &actual_len) == 0);
    assert(actual_len == frame_len);
    transport.tx_busy = false;

    transport.rx_busy = true;
    assert(m9p_uart_transport_send_frame(&transport, frame, frame_len) == 0);
    transport.rx_busy = false;
}

static void test_serve_once_round_trip(void)
{
    struct m9p_uart_transport transport;
    struct m9p_uart_transport_config config;
    uint8_t request[64];
    uint8_t expected_response[64];
    uint8_t handler_tx[64];
    uint8_t handler_rx[64];
    uint8_t payload[] = {0x44u, 0x66u};
    size_t request_len;
    size_t expected_len;
    size_t rx_len;
    size_t tx_len;

    fake_uart_reset();
    m9p_uart_transport_get_default_config(&config);
    config.flush_before_receive = false;
    assert(m9p_uart_transport_init(&transport, &config) == 0);

    assert(m9p_encode_frame(M9P_TWRITE,
                            0x444u,
                            payload,
                            (uint16_t)sizeof(payload),
                            request,
                            sizeof(request),
                            &request_len));
    assert(m9p_encode_frame(M9P_RWRITE,
                            0x444u,
                            payload,
                            (uint16_t)sizeof(payload),
                            expected_response,
                            sizeof(expected_response),
                            &expected_len));
    fake_uart_append_rx(request, request_len);

    assert(m9p_uart_transport_serve_once(&transport,
                                         server_handler,
                                         NULL,
                                         handler_rx,
                                         sizeof(handler_rx),
                                         &rx_len,
                                         handler_tx,
                                         sizeof(handler_tx),
                                         &tx_len) == 0);
    assert(rx_len == request_len);
    assert(tx_len == expected_len);
    assert(memcmp(g_uart.tx_buffer, expected_response, expected_len) == 0);
}

int main(void)
{
    test_send_frame();
    test_receive_frame();
    test_request_round_trip();
    test_direction_flags_are_independent();
    test_serve_once_round_trip();
    puts("slave uart_transport host tests passed");
    return 0;
}