#include "mesh_uart_transport.h"

#include <limits.h>
#include <string.h>

static struct mesh_uart_transport g_default_transport;

void mesh_uart_transport_get_default_config(struct mesh_uart_transport_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
#ifdef ESP_PLATFORM
    out_config->uart_port = MESH_UART_TRANSPORT_DEFAULT_PORT;
    out_config->tx_pin = MESH_UART_TRANSPORT_DEFAULT_TX_PIN;
    out_config->rx_pin = MESH_UART_TRANSPORT_DEFAULT_RX_PIN;
    out_config->baud_rate = MESH_UART_TRANSPORT_DEFAULT_BAUD_RATE;
    out_config->rx_buffer_size = MESH_UART_TRANSPORT_DEFAULT_RX_BUFFER_SIZE;
    out_config->tx_buffer_size = MESH_UART_TRANSPORT_DEFAULT_TX_BUFFER_SIZE;
#endif
    out_config->io_timeout_ms = MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS;
    out_config->flush_before_receive = false;
}

#if !defined(ESP_PLATFORM) && !defined(MESH_UART_TRANSPORT_USE_STM32_HAL)

int mesh_uart_transport_init(
    struct mesh_uart_transport *transport,
    const struct mesh_uart_transport_config *config)
{
    if (transport == NULL || config == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    transport->config = *config;
    transport->initialized = true;
    return 0;
}

void mesh_uart_transport_deinit(struct mesh_uart_transport *transport)
{
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
}

int mesh_uart_transport_send_frame(
    struct mesh_uart_transport *transport,
    const uint8_t *tx_data,
    size_t tx_len)
{
    (void)transport;
    (void)tx_data;
    (void)tx_len;
    return -(int)MESH_ERR_INVALID_STATE;
}

int mesh_uart_transport_receive_frame(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    (void)transport;
    (void)rx_data;
    (void)rx_cap;

    if (rx_len != NULL) {
        *rx_len = 0u;
    }
    return -(int)MESH_ERR_INVALID_STATE;
}

#elif defined(MESH_UART_TRANSPORT_USE_STM32_HAL)

static uint32_t transport_timeout_ms(const struct mesh_uart_transport *transport)
{
    if (transport->config.io_timeout_ms == 0u) {
        return 1u;
    }

    return transport->config.io_timeout_ms;
}

static int hal_status_to_mesh(HAL_StatusTypeDef status)
{
    switch (status) {
    case HAL_OK:
        return 0;
    case HAL_TIMEOUT:
    case HAL_BUSY:
        return -(int)MESH_ERR_BUSY;
    case HAL_ERROR:
    default:
        return -(int)MESH_ERR_INVALID_STATE;
    }
}

static int drain_rx_fifo(struct mesh_uart_transport *transport)
{
    uint8_t byte;

    while (HAL_UART_Receive(transport->config.uart, &byte, 1u, 1u) == HAL_OK) {
    }
    __HAL_UART_FLUSH_DRREGISTER(transport->config.uart);
    return 0;
}

static int read_exact(struct mesh_uart_transport *transport, uint8_t *buf, size_t len)
{
    size_t total = 0u;

    while (total < len) {
        size_t chunk = len - total;
        int rc;

        if (chunk > (size_t)UINT16_MAX) {
            chunk = (size_t)UINT16_MAX;
        }

        rc = hal_status_to_mesh(HAL_UART_Receive(
            transport->config.uart,
            buf + total,
            (uint16_t)chunk,
            transport_timeout_ms(transport)));
        if (rc != 0) {
            return rc;
        }

        total += chunk;
    }

    return 0;
}

static int receive_frame_locked(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    uint8_t header[4];
    uint16_t frame_len_field;
    size_t total_len;
    int rc;

    rc = read_exact(transport, header, sizeof(header));
    if (rc != 0) {
        return rc;
    }
    if (header[0] != (uint8_t)'M' || header[1] != (uint8_t)'H') {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    frame_len_field = (uint16_t)header[2] | (uint16_t)((uint16_t)header[3] << 8);
    if (frame_len_field < 8u) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    total_len = (size_t)frame_len_field + 6u;
    if (total_len > rx_cap || total_len < MESH_FRAME_OVERHEAD) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    memcpy(rx_data, header, sizeof(header));
    rc = read_exact(transport, rx_data + sizeof(header), total_len - sizeof(header));
    if (rc != 0) {
        return rc;
    }

    *rx_len = total_len;
    return 0;
}

int mesh_uart_transport_init(
    struct mesh_uart_transport *transport,
    const struct mesh_uart_transport_config *config)
{
    if (transport == NULL || config == NULL || config->uart == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    transport->config = *config;
    transport->initialized = true;
    return 0;
}

void mesh_uart_transport_deinit(struct mesh_uart_transport *transport)
{
    if (transport == NULL) {
        return;
    }

    memset(transport, 0, sizeof(*transport));
}

int mesh_uart_transport_send_frame(
    struct mesh_uart_transport *transport,
    const uint8_t *tx_data,
    size_t tx_len)
{
    if (transport == NULL || !transport->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (tx_len > (size_t)UINT16_MAX) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    return hal_status_to_mesh(HAL_UART_Transmit(
        transport->config.uart,
        (uint8_t *)tx_data,
        (uint16_t)tx_len,
        transport_timeout_ms(transport)));
}

int mesh_uart_transport_receive_frame(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    if (transport == NULL || !transport->initialized || rx_data == NULL || rx_len == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    *rx_len = 0u;

    if (transport->config.flush_before_receive) {
        int rc = drain_rx_fifo(transport);

        if (rc != 0) {
            return rc;
        }
    }

    return receive_frame_locked(transport, rx_data, rx_cap, rx_len);
}

#else

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static int transport_timeout_ms(const struct mesh_uart_transport *transport)
{
    if (transport->config.io_timeout_ms == 0u) {
        return 1;
    }

    return (int)transport->config.io_timeout_ms;
}

static TickType_t transport_timeout_ticks(const struct mesh_uart_transport *transport)
{
    TickType_t ticks = pdMS_TO_TICKS((TickType_t)transport_timeout_ms(transport));

    return ticks == 0 ? 1 : ticks;
}

static SemaphoreHandle_t transport_tx_lock(const struct mesh_uart_transport *transport)
{
    return (SemaphoreHandle_t)transport->tx_lock;
}

static SemaphoreHandle_t transport_rx_lock(const struct mesh_uart_transport *transport)
{
    return (SemaphoreHandle_t)transport->rx_lock;
}

static int transport_take_mutex(
    const struct mesh_uart_transport *transport,
    SemaphoreHandle_t lock)
{
    if (lock == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(lock, transport_timeout_ticks(transport)) != pdTRUE) {
        return -(int)MESH_ERR_BUSY;
    }

    return 0;
}

static void transport_give_mutex(SemaphoreHandle_t lock)
{
    if (lock != NULL) {
        xSemaphoreGive(lock);
    }
}

static void transport_delete_locks(struct mesh_uart_transport *transport)
{
    if (transport_tx_lock(transport) != NULL) {
        vSemaphoreDelete(transport_tx_lock(transport));
        transport->tx_lock = NULL;
    }
    if (transport_rx_lock(transport) != NULL) {
        vSemaphoreDelete(transport_rx_lock(transport));
        transport->rx_lock = NULL;
    }
}

static int esp_err_to_mesh(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return 0;
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_INVALID_STATE:
        return -(int)MESH_ERR_BUSY;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_NO_MEM:
    default:
        return -(int)MESH_ERR_INVALID_STATE;
    }
}

static int flush_input(struct mesh_uart_transport *transport)
{
    return esp_err_to_mesh(uart_flush_input((uart_port_t)transport->config.uart_port));
}

static int read_exact(struct mesh_uart_transport *transport, uint8_t *buf, size_t len)
{
    size_t total = 0u;

    while (total < len) {
        int read_now = uart_read_bytes(
            (uart_port_t)transport->config.uart_port,
            buf + total,
            len - total,
            transport_timeout_ticks(transport));

        if (read_now < 0) {
            return -(int)MESH_ERR_INVALID_STATE;
        }
        if (read_now == 0) {
            return -(int)MESH_ERR_BUSY;
        }

        total += (size_t)read_now;
    }

    return 0;
}

static int send_frame_locked(
    struct mesh_uart_transport *transport,
    const uint8_t *tx_data,
    size_t tx_len)
{
    int written;
    int rc;

    written = uart_write_bytes((uart_port_t)transport->config.uart_port, tx_data, tx_len);
    if (written < 0 || (size_t)written != tx_len) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = esp_err_to_mesh(uart_wait_tx_done(
        (uart_port_t)transport->config.uart_port,
        transport_timeout_ticks(transport)));
    return rc;
}

static int receive_frame_locked(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    uint8_t header[4];
    uint16_t frame_len_field;
    size_t total_len;
    int rc;

    rc = read_exact(transport, header, sizeof(header));
    if (rc != 0) {
        return rc;
    }
    if (header[0] != (uint8_t)'M' || header[1] != (uint8_t)'H') {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    frame_len_field = (uint16_t)header[2] | (uint16_t)((uint16_t)header[3] << 8);
    if (frame_len_field < 8u) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    total_len = (size_t)frame_len_field + 6u;
    if (total_len > rx_cap || total_len < MESH_FRAME_OVERHEAD) {
        return -(int)MESH_ERR_BAD_FRAME;
    }

    memcpy(rx_data, header, sizeof(header));
    rc = read_exact(transport, rx_data + sizeof(header), total_len - sizeof(header));
    if (rc != 0) {
        return rc;
    }

    *rx_len = total_len;
    return 0;
}

int mesh_uart_transport_init(
    struct mesh_uart_transport *transport,
    const struct mesh_uart_transport_config *config)
{
    uart_config_t uart_config;
    int rc;

    if (transport == NULL || config == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    memset(transport, 0, sizeof(*transport));
    transport->config = *config;

    transport->tx_lock = xSemaphoreCreateMutex();
    transport->rx_lock = xSemaphoreCreateMutex();
    if (transport_tx_lock(transport) == NULL || transport_rx_lock(transport) == NULL) {
        transport_delete_locks(transport);
        return -(int)MESH_ERR_BUSY;
    }

    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.baud_rate = config->baud_rate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    rc = esp_err_to_mesh(uart_param_config((uart_port_t)config->uart_port, &uart_config));
    if (rc != 0) {
        transport_delete_locks(transport);
        memset(transport, 0, sizeof(*transport));
        return rc;
    }

    rc = esp_err_to_mesh(uart_driver_install(
        (uart_port_t)config->uart_port,
        config->rx_buffer_size,
        config->tx_buffer_size,
        0,
        NULL,
        0));
    if (rc != 0) {
        transport_delete_locks(transport);
        memset(transport, 0, sizeof(*transport));
        return rc;
    }

    rc = esp_err_to_mesh(uart_set_pin(
        (uart_port_t)config->uart_port,
        config->tx_pin,
        config->rx_pin,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE));
    if (rc != 0) {
        (void)uart_driver_delete((uart_port_t)config->uart_port);
        transport_delete_locks(transport);
        memset(transport, 0, sizeof(*transport));
        return rc;
    }

    (void)uart_flush_input((uart_port_t)config->uart_port);
    transport->initialized = true;
    return 0;
}

void mesh_uart_transport_deinit(struct mesh_uart_transport *transport)
{
    if (transport == NULL || !transport->initialized) {
        return;
    }

    (void)uart_driver_delete((uart_port_t)transport->config.uart_port);
    transport_delete_locks(transport);
    memset(transport, 0, sizeof(*transport));
}

int mesh_uart_transport_send_frame(
    struct mesh_uart_transport *transport,
    const uint8_t *tx_data,
    size_t tx_len)
{
    int rc;

    if (transport == NULL || !transport->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = transport_take_mutex(transport, transport_tx_lock(transport));
    if (rc != 0) {
        return rc;
    }

    rc = send_frame_locked(transport, tx_data, tx_len);
    transport_give_mutex(transport_tx_lock(transport));
    return rc;
}

int mesh_uart_transport_receive_frame(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    int rc;

    if (transport == NULL || !transport->initialized || rx_data == NULL || rx_len == NULL ||
        rx_cap < MESH_FRAME_OVERHEAD) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    *rx_len = 0u;

    rc = transport_take_mutex(transport, transport_rx_lock(transport));
    if (rc != 0) {
        return rc;
    }

    if (transport->config.flush_before_receive) {
        rc = flush_input(transport);
        if (rc != 0) {
            transport_give_mutex(transport_rx_lock(transport));
            return rc;
        }
    }

    rc = receive_frame_locked(transport, rx_data, rx_cap, rx_len);
    transport_give_mutex(transport_rx_lock(transport));
    return rc;
}

#endif

int mesh_uart_transport_init_default(void)
{
    struct mesh_uart_transport_config config;

    if (g_default_transport.initialized) {
        return 0;
    }

    mesh_uart_transport_get_default_config(&config);
    return mesh_uart_transport_init(&g_default_transport, &config);
}

struct mesh_uart_transport *mesh_uart_transport_default(void)
{
    return &g_default_transport;
}