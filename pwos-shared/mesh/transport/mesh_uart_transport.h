#ifndef MESH_UART_TRANSPORT_H
#define MESH_UART_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../envelope/mesh_protocal.h"

#if defined(MESH_UART_TRANSPORT_USE_STM32_HAL)
#include "main.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_UART_TRANSPORT_DEFAULT_TIMEOUT_MS 200u

#ifdef ESP_PLATFORM
#define MESH_UART_TRANSPORT_DEFAULT_PORT 1
#define MESH_UART_TRANSPORT_DEFAULT_TX_PIN 17
#define MESH_UART_TRANSPORT_DEFAULT_RX_PIN 18
#define MESH_UART_TRANSPORT_DEFAULT_BAUD_RATE 1000000
#define MESH_UART_TRANSPORT_DEFAULT_RX_BUFFER_SIZE 1024u
#define MESH_UART_TRANSPORT_DEFAULT_TX_BUFFER_SIZE 1024u
#elif defined(MESH_UART_TRANSPORT_USE_STM32_HAL)
#define MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE 1024u
#define MESH_UART_TRANSPORT_STM32_FRAME_QUEUE_CAP 4u
#define MESH_UART_TRANSPORT_STM32_FRAME_CAP (MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN)
#endif

struct mesh_uart_transport_config {
#ifdef ESP_PLATFORM
    int uart_port;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    size_t rx_buffer_size;
    size_t tx_buffer_size;
#elif defined(MESH_UART_TRANSPORT_USE_STM32_HAL)
    UART_HandleTypeDef *uart;
#else
    void *uart;
#endif
    uint32_t io_timeout_ms;
    bool flush_before_receive;
};

struct mesh_uart_transport {
    struct mesh_uart_transport_config config;
#ifdef ESP_PLATFORM
    void *tx_lock;
    void *rx_lock;
#elif defined(MESH_UART_TRANSPORT_USE_STM32_HAL)
    uint8_t dma_rx_buffer[MESH_UART_TRANSPORT_STM32_DMA_RX_BUFFER_SIZE];
    uint16_t dma_last_pos;
    uint8_t parse_buffer[MESH_UART_TRANSPORT_STM32_FRAME_CAP];
    uint16_t parse_len;
    uint8_t frame_queue[MESH_UART_TRANSPORT_STM32_FRAME_QUEUE_CAP][MESH_UART_TRANSPORT_STM32_FRAME_CAP];
    uint16_t frame_lens[MESH_UART_TRANSPORT_STM32_FRAME_QUEUE_CAP];
    volatile uint8_t frame_head;
    volatile uint8_t frame_count;
    volatile uint32_t dropped_frames;
    volatile uint32_t bad_frames;
    bool dma_running;
#endif
    bool initialized;
};

void mesh_uart_transport_get_default_config(struct mesh_uart_transport_config *out_config);

int mesh_uart_transport_init(
    struct mesh_uart_transport *transport,
    const struct mesh_uart_transport_config *config);

void mesh_uart_transport_deinit(struct mesh_uart_transport *transport);

int mesh_uart_transport_send_frame(
    struct mesh_uart_transport *transport,
    const uint8_t *tx_data,
    size_t tx_len);

int mesh_uart_transport_receive_frame(
    struct mesh_uart_transport *transport,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len);

bool mesh_uart_transport_rx_pending(const struct mesh_uart_transport *transport);

int mesh_uart_transport_flush_input(struct mesh_uart_transport *transport);

int mesh_uart_transport_init_default(void);

struct mesh_uart_transport *mesh_uart_transport_default(void);

#ifdef __cplusplus
}
#endif

#endif
