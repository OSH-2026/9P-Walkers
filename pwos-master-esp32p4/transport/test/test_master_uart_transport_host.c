#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mini9p_protocol.h"
#include "uart_transport.h"

struct fake_mutex {
    bool locked;
};

struct fake_uart {
    uint8_t rx_buffer[512];
    size_t rx_len;
    size_t rx_pos;
    uint8_t tx_buffer[512];
    size_t tx_len;
    int flush_count;
    bool driver_installed;
    struct fake_mutex mutex;
    void (*on_write)(const uint8_t *data, size_t len);
};

static struct fake_uart g_uart;
static TickType_t g_ticks;
static uint8_t g_scripted_frame[128];
static size_t g_scripted_len;

static void fake_uart_reset(void)
{
    memset(&g_uart, 0, sizeof(g_uart));
    memset(g_scripted_frame, 0, sizeof(g_scripted_frame));
    g_scripted_len = 0u;
    g_ticks = 0u;
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
    uint8_t payload[] = {0x33u, 0x55u};
    uint16_t tag;

    (void)server_ctx;
    assert(request_len >= 8u);
    tag = read_le16(request_data + 6);
    assert(m9p_encode_frame(M9P_RSTAT,
                            tag,
                            payload,
                            (uint16_t)sizeof(payload),
                            response_data,
                            response_cap,
                            response_len));
    return 0;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    g_uart.mutex.locked = false;
    return &g_uart.mutex;
}

int xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
    struct fake_mutex *mutex = (struct fake_mutex *)xSemaphore;

    (void)xTicksToWait;
    if (mutex->locked) {
        return pdFALSE;
    }

    mutex->locked = true;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    struct fake_mutex *mutex = (struct fake_mutex *)xSemaphore;

    mutex->locked = false;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    struct fake_mutex *mutex = (struct fake_mutex *)xSemaphore;

    mutex->locked = false;
}

TickType_t xTaskGetTickCount(void)
{
    return g_ticks;
}

int uart_driver_install(uart_port_t uart_num,
                        int rx_buffer_size,
                        int tx_buffer_size,
                        int queue_size,
                        void *uart_queue,
                        int intr_alloc_flags)
{
    (void)uart_num;
    (void)rx_buffer_size;
    (void)tx_buffer_size;
    (void)queue_size;
    (void)uart_queue;
    (void)intr_alloc_flags;

    g_uart.driver_installed = true;
    return ESP_OK;
}

int uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config)
{
    (void)uart_num;
    (void)uart_config;
    return ESP_OK;
}

int uart_set_pin(uart_port_t uart_num,
                 int tx_io_num,
                 int rx_io_num,
                 int rts_io_num,
                 int cts_io_num)
{
    (void)uart_num;
    (void)tx_io_num;
    (void)rx_io_num;
    (void)rts_io_num;
    (void)cts_io_num;
    return ESP_OK;
}

int uart_flush_input(uart_port_t uart_num)
{
    (void)uart_num;
    g_uart.rx_len = 0u;
    g_uart.rx_pos = 0u;
    g_uart.flush_count += 1;
    return ESP_OK;
}

int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    (void)uart_num;
    assert(size <= sizeof(g_uart.tx_buffer));
    memcpy(g_uart.tx_buffer, src, size);
    g_uart.tx_len = size;
    if (g_uart.on_write != NULL) {
        g_uart.on_write((const uint8_t *)src, size);
    }
    return (int)size;
}

int uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait)
{
    (void)uart_num;
    (void)ticks_to_wait;
    return ESP_OK;
}

int uart_read_bytes(uart_port_t uart_num, void *buf, size_t length, TickType_t ticks_to_wait)
{
    size_t available;
    size_t chunk;

    (void)uart_num;
    (void)ticks_to_wait;
    if (g_uart.rx_pos >= g_uart.rx_len) {
        return 0;
    }

    available = g_uart.rx_len - g_uart.rx_pos;
    chunk = length < available ? length : available;
    memcpy(buf, g_uart.rx_buffer + g_uart.rx_pos, chunk);
    g_uart.rx_pos += chunk;
    return (int)chunk;
}

int uart_driver_delete(uart_port_t uart_num)
{
    (void)uart_num;
    g_uart.driver_installed = false;
    return ESP_OK;
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
    assert(m9p_encode_frame(M9P_TSTAT, 0x101u, NULL, 0u, frame, sizeof(frame), &frame_len));
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
    uint8_t payload[] = {0x10u, 0x20u, 0x30u};
    size_t expected_len;
    size_t actual_len;

    fake_uart_reset();
    m9p_uart_transport_get_default_config(&config);
    config.flush_before_receive = false;
    assert(m9p_uart_transport_init(&transport, &config) == 0);
    assert(m9p_encode_frame(M9P_RREAD,
                            0x202u,
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
    uint8_t payload[] = {0xABu, 0xCDu};
    size_t request_len;
    size_t response_len;
    size_t actual_len;

    fake_uart_reset();
    m9p_uart_transport_get_default_config(&config);
    assert(m9p_uart_transport_init(&transport, &config) == 0);

    assert(m9p_encode_frame(M9P_TREAD,
                            0x303u,
                            payload,
                            (uint16_t)sizeof(payload),
                            request,
                            sizeof(request),
                            &request_len));
    assert(m9p_encode_frame(M9P_RREAD,
                            0x303u,
                            payload,
                            (uint16_t)sizeof(payload),
                            response,
                            sizeof(response),
                            &response_len));
    memcpy(g_scripted_frame, response, response_len);
    g_scripted_len = response_len;
    g_uart.on_write = append_scripted_response;

    assert(m9p_uart_transport_request(&transport,
                                      request,
                                      request_len,
                                      actual,
                                      sizeof(actual),
                                      &actual_len) == 0);
    assert(g_uart.tx_len == request_len);
    assert(memcmp(g_uart.tx_buffer, request, request_len) == 0);
    assert(actual_len == response_len);
    assert(memcmp(actual, response, response_len) == 0);
}

static void test_serve_once_round_trip(void)
{
    struct m9p_uart_transport transport;
    struct m9p_uart_transport_config config;
    uint8_t request[64];
    uint8_t expected_response[64];
    uint8_t handler_tx[64];
    uint8_t handler_rx[64];
    uint8_t payload[] = {0x01u, 0x02u};
    uint8_t response_payload[] = {0x33u, 0x55u};
    size_t request_len;
    size_t expected_len;
    size_t rx_len;
    size_t tx_len;

    fake_uart_reset();
    m9p_uart_transport_get_default_config(&config);
    config.flush_before_receive = false;
    assert(m9p_uart_transport_init(&transport, &config) == 0);

    assert(m9p_encode_frame(M9P_TWRITE,
                            0x404u,
                            payload,
                            (uint16_t)sizeof(payload),
                            request,
                            sizeof(request),
                            &request_len));
    assert(m9p_encode_frame(M9P_RSTAT,
                            0x404u,
                            response_payload,
                            (uint16_t)sizeof(response_payload),
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
    test_serve_once_round_trip();
    puts("master uart_transport host tests passed");
    return 0;
}