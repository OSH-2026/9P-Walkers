#ifndef PWOS_FRAME_POOL_H
#define PWOS_FRAME_POOL_H

#include <stddef.h>
#include <stdint.h>

#include "pwos_link_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M2 固定帧块池容量。
 *
 * 24 块约占 13KB SRAM，用于 ISR、link、mesh、service 之间传递完整链路帧。
 * 队列只传 pwos_frame_block_t*，不复制整帧。
 */
#define PWOS_FRAME_POOL_CAPACITY 24u

typedef struct {
    uint8_t port_id;        /* 收到/准备发送该帧的物理端口 id */
    uint8_t reserved;       /* 保留，保持后续字段对齐 */
    uint16_t len;           /* data 中有效帧长度，最大 PWOS_LINK_MAX_FRAME_LEN */
    uint32_t timestamp_ms;  /* xTaskGetTickCount()/HAL tick 语义的时间戳 */
    uint32_t sequence;      /* 块分配序号，仅用于诊断 */
    uint8_t data[PWOS_LINK_MAX_FRAME_LEN];
} pwos_frame_block_t;

/*
 * 初始化固定块池。
 *
 * 调用上下文：scheduler 启动前调用一次；不可在 ISR 调用。
 */
void pwos_frame_pool_init(void);

/*
 * 从块池申请一个空帧块。
 *
 * 调用上下文：任务上下文；失败返回 NULL。
 */
pwos_frame_block_t *pwos_frame_pool_alloc(void);

/*
 * 从块池申请一个空帧块。
 *
 * 调用上下文：ISR；失败返回 NULL。内部只使用 FreeRTOS FromISR 临界区。
 */
pwos_frame_block_t *pwos_frame_pool_alloc_from_isr(void);

/*
 * 归还帧块。
 *
 * 调用上下文：任务上下文；block 为 NULL 或不属于本池时静默忽略。
 */
void pwos_frame_pool_free(pwos_frame_block_t *block);

/*
 * 归还帧块。
 *
 * 调用上下文：ISR；block 为 NULL 或不属于本池时静默忽略。
 */
void pwos_frame_pool_free_from_isr(pwos_frame_block_t *block);

/*
 * 返回当前空闲块数量。
 *
 * 调用上下文：任务上下文，主要用于 diag。
 */
size_t pwos_frame_pool_free_count(void);

/*
 * 返回块池总容量。
 *
 * 调用上下文：任意上下文。
 */
size_t pwos_frame_pool_capacity(void);

/*
 * 返回累计申请失败次数。
 *
 * 调用上下文：任务上下文，主要用于 diag。
 */
uint32_t pwos_frame_pool_alloc_fail_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_FRAME_POOL_H */
