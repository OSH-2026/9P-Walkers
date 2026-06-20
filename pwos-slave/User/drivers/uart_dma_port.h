#ifndef PWOS_UART_DMA_PORT_H
#define PWOS_UART_DMA_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_UART_DMA_RX_BUFFER_SIZE 1024u
#define PWOS_UART_DMA_MAX_PORTS 5u

typedef struct {
    uint8_t id;
    const char *name;
    UART_HandleTypeDef *huart;
} pwos_uart_dma_port_desc_t;

typedef struct {
    uint8_t id;
    uint8_t dma_running;
    uint16_t last_pos;
    uint32_t rx_bytes;
    uint32_t rx_frames;
    uint32_t rx_parse_errors;
    uint32_t rx_drop_no_block;
    uint32_t rx_drop_queue_full;
    uint32_t rx_rearm_deferred;
    uint32_t rx_rearm_failures;
    uint32_t uart_errors;
    uint32_t uart_last_error;
    uint32_t tx_bytes;
    uint32_t tx_frames;
    uint32_t tx_errors;
    uint32_t tx_timeouts;
    uint8_t tx_busy;
    uint32_t hal_errors;
    uint32_t dma_restarts;
} pwos_uart_dma_port_stats_t;

/*
 * 启动全部候选 UART 的 ReceiveToIdle DMA。
 *
 * 调用上下文：任务或 scheduler 启动前；必须在 pwos_queues_init() 后调用。
 * 返回：0 成功；负数表示至少一个端口启动失败。
 */
int pwos_uart_dma_ports_init(void);

/*
 * HAL_UARTEx_RxEventCallback 的实际处理函数。
 *
 * 调用上下文：ISR。消费本次 DMA buffer、link parser 喂字节、完整帧入队，并重启 RX DMA。
 */
void pwos_uart_dma_rx_event_from_isr(UART_HandleTypeDef *huart, uint16_t size);

/*
 * HAL_UART_ErrorCallback 的实际处理函数。
 *
 * 调用上下文：ISR。清理 parser 状态并尝试恢复 ReceiveToIdle DMA。
 */
void pwos_uart_dma_error_from_isr(UART_HandleTypeDef *huart);

/*
 * HAL_UART_TxCpltCallback 的实际处理函数。
 *
 * 调用上下文：ISR。只释放 TX 完成信号，不做业务逻辑。
 */
void pwos_uart_dma_tx_complete_from_isr(UART_HandleTypeDef *huart);

/*
 * 设置 RX DMA 重新 arm 任务。
 *
 * 调用上下文：scheduler 启动前。RX ISR 只投递通知，实际 HAL_UARTEx_ReceiveToIdle_DMA()
 * 在该任务里执行，避免在 HAL 回调栈内重启 DMA 导致 HAL_BUSY 或错误计数暴涨。
 */
void pwos_uart_dma_set_recover_task(TaskHandle_t task);

/*
 * 通过指定端口发送一帧完整 link frame。
 *
 * 调用上下文：link_tx_task。data 在函数返回前必须保持有效。
 * 返回：0 成功；负数表示参数错误、HAL 启动失败或 TX 超时。
 */
int pwos_uart_dma_send(
    uint8_t port_id,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms);

/*
 * 轮询式恢复入口。
 *
 * 调用上下文：port_mgr_task。用于处理 ISR 里 HAL_BUSY 导致的延迟重启。
 */
void pwos_uart_dma_poll_recover(void);

size_t pwos_uart_dma_port_count(void);
const pwos_uart_dma_port_desc_t *pwos_uart_dma_port_desc(size_t index);
int pwos_uart_dma_get_stats(size_t index, pwos_uart_dma_port_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_UART_DMA_PORT_H */
