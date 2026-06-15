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

bool mesh_uart_transport_rx_pending(const struct mesh_uart_transport *transport)
{
    (void)transport;
    return true;
}

int mesh_uart_transport_flush_input(struct mesh_uart_transport *transport)
{
    (void)transport;
    return 0;
}

#elif defined(MESH_UART_TRANSPORT_USE_STM32_HAL)

#define MESH_UART_TRANSPORT_STM32_MAX_INSTANCES 8u

static struct mesh_uart_transport *g_stm32_transports[MESH_UART_TRANSPORT_STM32_MAX_INSTANCES];

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

static uint32_t mesh_uart_transport_enter_critical(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

static void mesh_uart_transport_exit_critical(uint32_t primask)
{
    if ((primask & 1u) == 0u) {
        __enable_irq();
    }
}

static struct mesh_uart_transport *stm32_find_transport(UART_HandleTypeDef *uart)
{
    size_t i;

    if (uart == NULL) {
        return NULL;
    }

    for (i = 0u; i < MESH_UART_TRANSPORT_STM32_MAX_INSTANCES; ++i) {
        if (g_stm32_transports[i] != NULL &&
            g_stm32_transports[i]->initialized &&
            g_stm32_transports[i]->config.uart == uart) {
            return g_stm32_transports[i];
        }
    }

    return NULL;
}

static int stm32_register_transport(struct mesh_uart_transport *transport)
{
    size_t i;

    if (transport == NULL || transport->config.uart == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    for (i = 0u; i < MESH_UART_TRANSPORT_STM32_MAX_INSTANCES; ++i) {
        if (g_stm32_transports[i] == transport) {
            return 0;
        }
        if (g_stm32_transports[i] != NULL &&
            g_stm32_transports[i]->config.uart == transport->config.uart) {
            g_stm32_transports[i] = transport;
            return 0;
        }
    }

    for (i = 0u; i < MESH_UART_TRANSPORT_STM32_MAX_INSTANCES; ++i) {
        if (g_stm32_transports[i] == NULL) {
            g_stm32_transports[i] = transport;
            return 0;
        }
    }

    return -(int)MESH_ERR_BUSY;
}

static void stm32_unregister_transport(struct mesh_uart_transport *transport)
{
    size_t i;

    for (i = 0u; i < MESH_UART_TRANSPORT_STM32_MAX_INSTANCES; ++i) {
        if (g_stm32_transports[i] == transport) {
            g_stm32_transports[i] = NULL;
        }
    }
}

static uint16_t stm32_dma_current_pos(const struct mesh_uart_transport *transport)
{
    uint32_t remaining;
    uint32_t pos;

    if (transport == NULL ||
        transport->config.uart == NULL ||
        transport->config.uart->hdmarx == NULL) {
        return 0u;
    }

    remaining = __HAL_DMA_GET_COUNTER(transport->config.uart->hdmarx);
    if (remaining > MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE) {
        return 0u;
    }

    pos = MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE - remaining;
    if (pos > MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE) {
        pos = 0u;
    }
    return (uint16_t)pos;
}

static void stm32_reset_rx_state(struct mesh_uart_transport *transport)
{
    uint32_t primask;

    if (transport == NULL) {
        return;
    }

    primask = mesh_uart_transport_enter_critical();
    transport->dma_last_pos = stm32_dma_current_pos(transport);
    transport->parse_len = 0u;
    transport->frame_head = 0u;
    transport->frame_count = 0u;
    mesh_uart_transport_exit_critical(primask);
}

static void stm32_enqueue_frame_from_isr(
    struct mesh_uart_transport *transport,
    const uint8_t *frame,
    uint16_t frame_len)
{
    uint8_t slot;

    if (transport == NULL || frame == NULL || frame_len == 0u ||
        frame_len > MESH_UART_TRANSPORT_STM32_FRAME_CAP) {
        return;
    }

    if (transport->frame_count >= MESH_UART_TRANSPORT_STM32_FRAME_QUEUE_CAP) {
        ++transport->dropped_frames;
        return;
    }

    slot = (uint8_t)((transport->frame_head + transport->frame_count) %
                    MESH_UART_TRANSPORT_STM32_FRAME_QUEUE_CAP);
    memcpy(transport->frame_queue[slot], frame, frame_len);
    transport->frame_lens[slot] = frame_len;
    ++transport->frame_count;
}

static void stm32_parser_reset(struct mesh_uart_transport *transport)
{
    if (transport != NULL) {
        transport->parse_len = 0u;
    }
}

static void stm32_consume_byte_from_isr(struct mesh_uart_transport *transport, uint8_t byte)
{
    uint16_t frame_len_field;
    uint16_t expected_len;

    if (transport == NULL) {
        return;
    }

    if (transport->parse_len == 0u) {
        if (byte == (uint8_t)'M') {
            transport->parse_buffer[0] = byte;
            transport->parse_len = 1u;
        }
        return;
    }

    if (transport->parse_len == 1u) {
        if (byte == (uint8_t)'H') {
            transport->parse_buffer[1] = byte;
            transport->parse_len = 2u;
            return;
        }

        transport->parse_len = 0u;
        if (byte == (uint8_t)'M') {
            transport->parse_buffer[0] = byte;
            transport->parse_len = 1u;
        }
        return;
    }

    if (transport->parse_len >= MESH_UART_TRANSPORT_STM32_FRAME_CAP) {
        ++transport->bad_frames;
        stm32_parser_reset(transport);
        return;
    }

    transport->parse_buffer[transport->parse_len++] = byte;

    if (transport->parse_len < 4u) {
        return;
    }

    frame_len_field = (uint16_t)transport->parse_buffer[2] |
        (uint16_t)((uint16_t)transport->parse_buffer[3] << 8);
    if (frame_len_field < 8u || frame_len_field > (uint16_t)(8u + MESH_MAX_PAYLOAD_LEN)) {
        ++transport->bad_frames;
        stm32_parser_reset(transport);
        return;
    }

    expected_len = (uint16_t)(frame_len_field + 6u);
    if (expected_len > MESH_UART_TRANSPORT_STM32_FRAME_CAP) {
        ++transport->bad_frames;
        stm32_parser_reset(transport);
        return;
    }

    if (transport->parse_len == expected_len) {
        stm32_enqueue_frame_from_isr(transport, transport->parse_buffer, expected_len);
        stm32_parser_reset(transport);
    } else if (transport->parse_len > expected_len) {
        ++transport->bad_frames;
        stm32_parser_reset(transport);
    }
}

static void stm32_consume_span_from_isr(
    struct mesh_uart_transport *transport,
    const uint8_t *data,
    uint16_t len)
{
    uint16_t i;

    if (transport == NULL || data == NULL) {
        return;
    }

    for (i = 0u; i < len; ++i) {
        stm32_consume_byte_from_isr(transport, data[i]);
    }
}

static void stm32_dma_consume_until_from_isr(
    struct mesh_uart_transport *transport,
    uint16_t pos)
{
    uint16_t last;

    if (transport == NULL || !transport->dma_running) {
        return;
    }

    if (pos > MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE) {
        pos = MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE;
    }

    last = transport->dma_last_pos;
    if (last > MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE) {
        last = 0u;
    }
    if (pos == last) {
        return;
    }

    if (pos > last) {
        stm32_consume_span_from_isr(
            transport,
            transport->dma_rx_buffer + last,
            (uint16_t)(pos - last));
    } else {
        stm32_consume_span_from_isr(
            transport,
            transport->dma_rx_buffer + last,
            (uint16_t)(MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE - last));
        if (pos > 0u) {
            stm32_consume_span_from_isr(transport, transport->dma_rx_buffer, pos);
        }
    }

    transport->dma_last_pos = pos;
}

static void stm32_poll_dma_rx(struct mesh_uart_transport *transport)
{
    uint32_t primask;
    uint16_t pos;

    if (transport == NULL || !transport->dma_running) {
        return;
    }

    pos = stm32_dma_current_pos(transport);
    primask = mesh_uart_transport_enter_critical();
    stm32_dma_consume_until_from_isr(transport, pos);
    mesh_uart_transport_exit_critical(primask);
}

static int stm32_start_dma_rx(struct mesh_uart_transport *transport)
{
    HAL_StatusTypeDef status;

    if (transport == NULL || transport->config.uart == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    transport->dma_last_pos = 0u;
    status = HAL_UARTEx_ReceiveToIdle_DMA(
        transport->config.uart,
        transport->dma_rx_buffer,
        MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE);
    if (status != HAL_OK) {
        transport->dma_running = false;
        return hal_status_to_mesh(status);
    }

    if (transport->config.uart->hdmarx != NULL) {
        __HAL_DMA_DISABLE_IT(transport->config.uart->hdmarx, DMA_IT_HT);
    }
    transport->dma_running = true;
    return 0;
}

static int stm32_dequeue_frame(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    uint32_t primask;
    uint8_t slot;
    uint16_t frame_len;

    if (transport == NULL || rx_data == NULL || rx_len == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    primask = mesh_uart_transport_enter_critical();
    if (transport->frame_count == 0u) {
        mesh_uart_transport_exit_critical(primask);
        return -(int)MESH_ERR_BUSY;
    }

    slot = transport->frame_head;
    frame_len = transport->frame_lens[slot];
    if (frame_len == 0u ||
        frame_len > MESH_UART_TRANSPORT_STM32_FRAME_CAP ||
        (size_t)frame_len > rx_cap) {
        transport->frame_head =
            (uint8_t)((transport->frame_head + 1u) % MESH_UART_TRANSPORT_STM32_FRAME_QUEUE_CAP);
        --transport->frame_count;
        mesh_uart_transport_exit_critical(primask);
        *rx_len = 0u;
        return -(int)MESH_ERR_BAD_FRAME;
    }

    memcpy(rx_data, transport->frame_queue[slot], frame_len);
    transport->frame_head =
        (uint8_t)((transport->frame_head + 1u) % MESH_UART_TRANSPORT_STM32_FRAME_QUEUE_CAP);
    --transport->frame_count;
    mesh_uart_transport_exit_critical(primask);

    *rx_len = frame_len;
    return 0;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    struct mesh_uart_transport *transport = stm32_find_transport(huart);

    if (transport == NULL) {
        return;
    }
    stm32_dma_consume_until_from_isr(transport, Size);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    struct mesh_uart_transport *transport = stm32_find_transport(huart);

    if (transport == NULL) {
        return;
    }

    transport->dma_running = false;
    transport->parse_len = 0u;
    transport->dma_last_pos = 0u;
    huart->ErrorCode = HAL_UART_ERROR_NONE;
    (void)stm32_start_dma_rx(transport);
}

int mesh_uart_transport_init(
    struct mesh_uart_transport *transport,
    const struct mesh_uart_transport_config *config)
{
    struct mesh_uart_transport_config local_config;
    int rc;

    if (transport == NULL || config == NULL || config->uart == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    local_config = *config;
    memset(transport, 0, sizeof(*transport));
    transport->config = local_config;
    transport->initialized = true;
    rc = stm32_register_transport(transport);
    if (rc != 0) {
        memset(transport, 0, sizeof(*transport));
        return rc;
    }

    rc = stm32_start_dma_rx(transport);
    if (rc != 0) {
        stm32_unregister_transport(transport);
        memset(transport, 0, sizeof(*transport));
        return rc;
    }

    return 0;
}

void mesh_uart_transport_deinit(struct mesh_uart_transport *transport)
{
    if (transport == NULL) {
        return;
    }

    if (transport->initialized && transport->config.uart != NULL) {
        transport->dma_running = false;
        (void)HAL_UART_DMAStop(transport->config.uart);
        stm32_unregister_transport(transport);
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
    stm32_poll_dma_rx(transport);

    if (transport->config.flush_before_receive) {
        stm32_reset_rx_state(transport);
    }

    return stm32_dequeue_frame(transport, rx_data, rx_cap, rx_len);
}

bool mesh_uart_transport_rx_pending(const struct mesh_uart_transport *transport)
{
    struct mesh_uart_transport *mutable_transport = (struct mesh_uart_transport *)transport;

    if (transport == NULL || !transport->initialized || transport->config.uart == NULL) {
        return false;
    }

    stm32_poll_dma_rx(mutable_transport);
    return mutable_transport->frame_count > 0u;
}

int mesh_uart_transport_flush_input(struct mesh_uart_transport *transport)
{
    if (transport == NULL || !transport->initialized || transport->config.uart == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    stm32_reset_rx_state(transport);
    return 0;
}

#else

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mesh_uart";

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
    ESP_LOGI(
        TAG,
        "initialized uart%d tx=%d rx=%d baud=%d timeout=%u ms",
        config->uart_port,
        config->tx_pin,
        config->rx_pin,
        config->baud_rate,
        (unsigned)config->io_timeout_ms);
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

bool mesh_uart_transport_rx_pending(const struct mesh_uart_transport *transport)
{
    size_t buffered = 0u;

    if (transport == NULL || !transport->initialized) {
        return false;
    }
    if (uart_get_buffered_data_len((uart_port_t)transport->config.uart_port, &buffered) != ESP_OK) {
        return false;
    }

    return buffered > 0u;
}

int mesh_uart_transport_flush_input(struct mesh_uart_transport *transport)
{
    if (transport == NULL || !transport->initialized) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return flush_input(transport);
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
