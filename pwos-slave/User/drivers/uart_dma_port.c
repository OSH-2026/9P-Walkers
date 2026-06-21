#include "uart_dma_port.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "frame_pool.h"
#include "pwos_link_parser.h"
#include "pwos_queues.h"
#include "usart.h"

#define PWOS_UART_DMA_PORT_COUNT PWOS_UART_DMA_MAX_PORTS

typedef struct {
    pwos_uart_dma_port_desc_t desc;
    uint8_t *rx_buffer;
    pwos_link_parser_t parser;
    uint16_t last_pos;
    uint8_t dma_running;
    uint8_t restart_pending;
    uint8_t tx_in_flight;
    StaticSemaphore_t tx_done_cb;
    SemaphoreHandle_t tx_done;
    pwos_uart_dma_port_stats_t stats;
} pwos_uart_dma_port_t;

/*
 * F407 的 CCMRAM 不能被 DMA 访问，因此 DMA buffer 必须留在普通 SRAM。
 */
static uint8_t g_uart_dma_rx_buffers[PWOS_UART_DMA_PORT_COUNT][PWOS_UART_DMA_RX_BUFFER_SIZE]
    __attribute__((aligned(4)));

static TaskHandle_t g_recover_task;

static pwos_uart_dma_port_t g_ports[PWOS_UART_DMA_PORT_COUNT] = {
    { .desc = { .id = 0u, .name = "USART1", .huart = &huart1 }, .rx_buffer = g_uart_dma_rx_buffers[0] },
    { .desc = { .id = 1u, .name = "USART2", .huart = &huart2 }, .rx_buffer = g_uart_dma_rx_buffers[1] },
    { .desc = { .id = 2u, .name = "USART3", .huart = &huart3 }, .rx_buffer = g_uart_dma_rx_buffers[2] },
    { .desc = { .id = 3u, .name = "UART4", .huart = &huart4 }, .rx_buffer = g_uart_dma_rx_buffers[3] },
    { .desc = { .id = 4u, .name = "USART6", .huart = &huart6 }, .rx_buffer = g_uart_dma_rx_buffers[4] },
};

static void snapshot_uart_state(pwos_uart_dma_port_t *port)
{
    UART_HandleTypeDef *huart;

    if (port == NULL || port->desc.huart == NULL || port->desc.huart->Instance == NULL) {
        return;
    }

    /*
     * 只读取寄存器，不读 DR；避免诊断代码误清 RXNE/IDLE/错误标志。
     * 真正清标志仍交给 HAL IRQ 和 error 恢复路径。
     */
    huart = port->desc.huart;
    port->stats.uart_last_sr = huart->Instance->SR;
    port->stats.uart_last_cr1 = huart->Instance->CR1;
    port->stats.uart_last_cr3 = huart->Instance->CR3;
    port->stats.uart_last_rx_state = (uint32_t)huart->RxState;
    port->stats.uart_last_g_state = (uint32_t)huart->gState;
}

static pwos_uart_dma_port_t *find_port_by_huart(UART_HandleTypeDef *huart)
{
    size_t i;

    if (huart == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_UART_DMA_PORT_COUNT; ++i) {
        if (g_ports[i].desc.huart == huart) {
            return &g_ports[i];
        }
    }
    return NULL;
}

static pwos_uart_dma_port_t *find_port_by_id(uint8_t port_id)
{
    size_t i;

    for (i = 0u; i < PWOS_UART_DMA_PORT_COUNT; ++i) {
        if (g_ports[i].desc.id == port_id) {
            return &g_ports[i];
        }
    }
    return NULL;
}

static HAL_StatusTypeDef start_port_dma(pwos_uart_dma_port_t *port, uint8_t reset_parser)
{
    HAL_StatusTypeDef status;

    if (port == NULL || port->desc.huart == NULL || port->desc.huart->hdmarx == NULL) {
        return HAL_ERROR;
    }

    port->last_pos = 0u;
    if (reset_parser != 0u) {
        /*
         * DMA 正常重启不能清 parser，否则跨 DMA 事件但尚未完成的 link frame 会被截断。
         * 只有初始化或 UART 错误恢复时才重置协议解析状态。
         */
        pwos_link_parser_reset(&port->parser);
    }

    status = HAL_UARTEx_ReceiveToIdle_DMA(
        port->desc.huart,
        port->rx_buffer,
        (uint16_t)PWOS_UART_DMA_RX_BUFFER_SIZE);
    port->stats.hal_last_status = (uint32_t)status;
    snapshot_uart_state(port);
    if (status == HAL_OK) {
        /*
         * RX 使用 DMA_NORMAL + ReceiveToIdle。HT 对本协议没有价值，TC 保留用于
         * 一次输入填满 1024B buffer 的场景；IDLE/TC 回调后会立即重新启动接收。
         */
        __HAL_DMA_DISABLE_IT(port->desc.huart->hdmarx, DMA_IT_HT);
        port->dma_running = 1u;
        port->restart_pending = 0u;
        port->stats.dma_running = 1u;
        port->stats.last_pos = 0u;
        ++port->stats.dma_restarts;
    } else {
        port->dma_running = 0u;
        port->restart_pending = 1u;
        port->stats.dma_running = 0u;
    }

    return status;
}

static void notify_recover_task_from_isr(BaseType_t *higher_priority_task_woken)
{
    if (g_recover_task != NULL &&
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskNotifyGiveFromISR(g_recover_task, higher_priority_task_woken);
    }
}

static void request_rx_rearm_from_isr(
    pwos_uart_dma_port_t *port,
    BaseType_t *higher_priority_task_woken)
{
    if (port == NULL) {
        return;
    }

    port->dma_running = 0u;
    port->restart_pending = 1u;
    port->stats.dma_running = 0u;
    ++port->stats.rx_rearm_deferred;
    notify_recover_task_from_isr(higher_priority_task_woken);
}

static void copy_frame_to_queue_from_isr(
    pwos_uart_dma_port_t *port,
    const pwos_link_parse_event_t *event,
    BaseType_t *higher_priority_task_woken)
{
    pwos_frame_block_t *block;

    if (port == NULL || event == NULL || event->raw_frame == NULL ||
        event->raw_frame_len == 0u ||
        event->raw_frame_len > (size_t)PWOS_LINK_MAX_FRAME_LEN) {
        ++port->stats.rx_parse_errors;
        return;
    }

    block = pwos_frame_pool_alloc_from_isr();
    if (block == NULL) {
        ++port->stats.rx_drop_no_block;
        return;
    }

    block->port_id = port->desc.id;
    block->len = (uint16_t)event->raw_frame_len;
    block->timestamp_ms = (uint32_t)xTaskGetTickCountFromISR();
    memcpy(block->data, event->raw_frame, event->raw_frame_len);

    if (pwos_link_rx_send_from_isr(block, higher_priority_task_woken) != pdPASS) {
        ++port->stats.rx_drop_queue_full;
        pwos_frame_pool_free_from_isr(block);
        return;
    }

    ++port->stats.rx_frames;
}

static void consume_bytes_from_isr(
    pwos_uart_dma_port_t *port,
    const uint8_t *data,
    size_t len,
    BaseType_t *higher_priority_task_woken)
{
    size_t i;

    if (port == NULL || data == NULL || len == 0u) {
        return;
    }

    port->stats.rx_bytes += (uint32_t)len;

    for (i = 0u; i < len; ++i) {
        pwos_link_parse_event_t event;
        pwos_link_parse_kind_t kind;

        kind = pwos_link_parser_feed_byte(&port->parser, data[i], &event);
        if (kind == PWOS_LINK_PARSE_FRAME) {
            copy_frame_to_queue_from_isr(port, &event, higher_priority_task_woken);
        } else if (kind == PWOS_LINK_PARSE_ERROR) {
            ++port->stats.rx_parse_errors;
        }
    }
}

static void consume_dma_region_from_isr(
    pwos_uart_dma_port_t *port,
    uint16_t from,
    uint16_t to,
    BaseType_t *higher_priority_task_woken)
{
    if (port == NULL || from >= PWOS_UART_DMA_RX_BUFFER_SIZE || to > PWOS_UART_DMA_RX_BUFFER_SIZE ||
        to <= from) {
        return;
    }

    consume_bytes_from_isr(
        port,
        &port->rx_buffer[from],
        (size_t)(to - from),
        higher_priority_task_woken);
}

int pwos_uart_dma_ports_init(void)
{
    size_t i;
    int failed = 0;

    for (i = 0u; i < PWOS_UART_DMA_PORT_COUNT; ++i) {
        memset(&g_ports[i].stats, 0, sizeof(g_ports[i].stats));
        g_ports[i].stats.id = g_ports[i].desc.id;
        pwos_link_parser_init(&g_ports[i].parser);
        g_ports[i].last_pos = 0u;
        g_ports[i].dma_running = 0u;
        g_ports[i].restart_pending = 0u;
        g_ports[i].tx_in_flight = 0u;
        g_ports[i].tx_done = xSemaphoreCreateBinaryStatic(&g_ports[i].tx_done_cb);

        if (start_port_dma(&g_ports[i], 1u) != HAL_OK) {
            ++g_ports[i].stats.hal_errors;
            failed = 1;
        }
        if (g_ports[i].tx_done == NULL) {
            failed = 1;
        }
    }

    return failed ? -1 : 0;
}

void pwos_uart_dma_rx_event_from_isr(UART_HandleTypeDef *huart, uint16_t size)
{
    pwos_uart_dma_port_t *port;
    BaseType_t higher_priority_task_woken = pdFALSE;
    HAL_UART_RxEventTypeTypeDef event_type;

    port = find_port_by_huart(huart);
    if (port == NULL || port->rx_buffer == NULL) {
        return;
    }

    port->dma_running = 1u;
    event_type = HAL_UARTEx_GetRxEventType(huart);
    ++port->stats.rx_events;
    port->stats.rx_last_event_size = size;
    port->stats.rx_last_event_type = (uint32_t)event_type;
    snapshot_uart_state(port);

    if (size > PWOS_UART_DMA_RX_BUFFER_SIZE) {
        size = (uint16_t)PWOS_UART_DMA_RX_BUFFER_SIZE;
    }
    if (size > 0u) {
        consume_dma_region_from_isr(port, 0u, size, &higher_priority_task_woken);
    }

    /*
     * HAL 在 DMA_NORMAL 模式下收到 IDLE/TC 后会结束本次接收。
     * 不能在 HAL 回调栈内马上重启 DMA：DMA IRQ 路径里 HAL 可能尚未完全收尾，
     * 直接重启会偶发 HAL_BUSY。这里只标记并通知 uart_rx 任务重新 arm。
     */
    (void)event_type;
    port->last_pos = size;
    port->stats.last_pos = size;
    request_rx_rearm_from_isr(port, &higher_priority_task_woken);

    if (higher_priority_task_woken != pdFALSE &&
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

void pwos_uart_dma_error_from_isr(UART_HandleTypeDef *huart)
{
    pwos_uart_dma_port_t *port;
    BaseType_t higher_priority_task_woken = pdFALSE;

    port = find_port_by_huart(huart);
    if (port == NULL) {
        return;
    }

    ++port->stats.hal_errors;
    ++port->stats.uart_errors;
    port->stats.uart_last_error = huart->ErrorCode;
    snapshot_uart_state(port);
    port->dma_running = 0u;
    port->restart_pending = 1u;
    port->last_pos = 0u;
    port->stats.dma_running = 0u;
    port->stats.last_pos = 0u;
    pwos_link_parser_reset(&port->parser);

    __HAL_UART_CLEAR_OREFLAG(huart);
    huart->ErrorCode = HAL_UART_ERROR_NONE;

    request_rx_rearm_from_isr(port, &higher_priority_task_woken);

    if (higher_priority_task_woken != pdFALSE &&
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

void pwos_uart_dma_tx_complete_from_isr(UART_HandleTypeDef *huart)
{
    pwos_uart_dma_port_t *port;
    BaseType_t higher_priority_task_woken = pdFALSE;

    port = find_port_by_huart(huart);
    if (port == NULL) {
        return;
    }

    port->tx_in_flight = 0u;
    port->stats.tx_busy = 0u;
    if (port->tx_done != NULL) {
        (void)xSemaphoreGiveFromISR(port->tx_done, &higher_priority_task_woken);
    }

    if (higher_priority_task_woken != pdFALSE &&
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

int pwos_uart_dma_send(
    uint8_t port_id,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms)
{
    pwos_uart_dma_port_t *port;
    HAL_StatusTypeDef status;
    TickType_t timeout_ticks;

    port = find_port_by_id(port_id);
    if (port == NULL || port->desc.huart == NULL || data == NULL ||
        len == 0u || len > (size_t)UINT16_MAX || port->tx_done == NULL) {
        return -1;
    }

    taskENTER_CRITICAL();
    if (port->tx_in_flight != 0u) {
        taskEXIT_CRITICAL();
        ++port->stats.tx_errors;
        return -2;
    }
    port->tx_in_flight = 1u;
    port->stats.tx_busy = 1u;
    taskEXIT_CRITICAL();

    while (xSemaphoreTake(port->tx_done, 0u) == pdPASS) {
    }

    status = HAL_UART_Transmit_DMA(port->desc.huart, (uint8_t *)data, (uint16_t)len);
    if (status != HAL_OK) {
        taskENTER_CRITICAL();
        port->tx_in_flight = 0u;
        port->stats.tx_busy = 0u;
        taskEXIT_CRITICAL();
        ++port->stats.tx_errors;
        return -3;
    }

    timeout_ticks = timeout_ms == 0u ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(port->tx_done, timeout_ticks) != pdPASS) {
        (void)HAL_UART_AbortTransmit(port->desc.huart);
        taskENTER_CRITICAL();
        port->tx_in_flight = 0u;
        port->stats.tx_busy = 0u;
        taskEXIT_CRITICAL();
        ++port->stats.tx_timeouts;
        return -4;
    }

    port->stats.tx_bytes += (uint32_t)len;
    ++port->stats.tx_frames;
    return 0;
}

void pwos_uart_dma_set_recover_task(TaskHandle_t task)
{
    taskENTER_CRITICAL();
    g_recover_task = task;
    taskEXIT_CRITICAL();
}

void pwos_uart_dma_poll_recover(void)
{
    size_t i;

    for (i = 0u; i < PWOS_UART_DMA_PORT_COUNT; ++i) {
        if (g_ports[i].restart_pending != 0u) {
            if (start_port_dma(&g_ports[i], 0u) != HAL_OK) {
                ++g_ports[i].stats.hal_errors;
                ++g_ports[i].stats.rx_rearm_failures;
            }
        }
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    pwos_uart_dma_rx_event_from_isr(huart, size);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    pwos_uart_dma_error_from_isr(huart);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    pwos_uart_dma_tx_complete_from_isr(huart);
}

size_t pwos_uart_dma_port_count(void)
{
    return PWOS_UART_DMA_PORT_COUNT;
}

const pwos_uart_dma_port_desc_t *pwos_uart_dma_port_desc(size_t index)
{
    if (index >= PWOS_UART_DMA_PORT_COUNT) {
        return NULL;
    }
    return &g_ports[index].desc;
}

int pwos_uart_dma_get_stats(size_t index, pwos_uart_dma_port_stats_t *out_stats)
{
    if (index >= PWOS_UART_DMA_PORT_COUNT || out_stats == NULL) {
        return -1;
    }

    taskENTER_CRITICAL();
    g_ports[index].stats.dma_running = g_ports[index].dma_running;
    g_ports[index].stats.last_pos = g_ports[index].last_pos;
    g_ports[index].stats.tx_busy = g_ports[index].tx_in_flight;
    *out_stats = g_ports[index].stats;
    taskEXIT_CRITICAL();

    return 0;
}
