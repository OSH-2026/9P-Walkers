/*
 * pwos_queues.c - PWOS M2 队列封装实现
 *
 * 本文件实现 5 条 FreeRTOS 静态队列，用于在 ISR、link_rx_task、
 * mesh_ctrl_task、service_task、link_tx_task 之间传递 pwos_frame_block_t 指针。
 *
 * 内存布局
 * --------
 * 每条队列需要两块静态内存：
 *   - StaticQueue_t：FreeRTOS 内部控制块。
 *   - uint8_t[]：队列存储区，大小 = 队列深度 × sizeof(pwos_frame_block_t *)，
 *     按 4 字节对齐。
 *
 * 线程/ISR 安全
 * -------------
 *   - link_rx 队列：只应由 ISR 调用 send_from_isr，任务调用 receive。
 *   - mesh_rx / ctrl_tx / link_tx 队列：只应由任务调用 send/receive。
 *   - 不要混用 send 和 send_from_isr，避免中断嵌套问题。
 *
 * 错误处理
 * --------
 * 所有 send 接口在失败时都会累加对应的 drop 计数器，并返回 pdFALSE。
 * 调用方在 send 失败后仍拥有 block，必须决定是否重试或释放，避免内存泄漏。
 */

#include "pwos_queues.h"

/*
 * 计算队列存储区字节数。
 *
 * 队列元素是 pwos_frame_block_t 指针，因此大小为 队列长度 × 指针大小。
 */
#define PWOS_QUEUE_STORAGE_BYTES(queue_len) ((queue_len) * sizeof(pwos_frame_block_t *))

/* FreeRTOS 静态队列控制块。 */
static StaticQueue_t g_link_rx_queue_cb;
static StaticQueue_t g_mesh_rx_queue_cb;
static StaticQueue_t g_service_rx_queue_cb;
static StaticQueue_t g_ctrl_tx_queue_cb;
static StaticQueue_t g_link_tx_queue_cb;

/* 队列存储区，按 4 字节对齐以满足 FreeRTOS 要求。 */
static uint8_t g_link_rx_queue_storage[PWOS_QUEUE_STORAGE_BYTES(PWOS_LINK_RX_QUEUE_LEN)]
    __attribute__((aligned(4)));
static uint8_t g_mesh_rx_queue_storage[PWOS_QUEUE_STORAGE_BYTES(PWOS_MESH_RX_QUEUE_LEN)]
    __attribute__((aligned(4)));
static uint8_t g_service_rx_queue_storage[PWOS_QUEUE_STORAGE_BYTES(PWOS_SERVICE_RX_QUEUE_LEN)]
    __attribute__((aligned(4)));
static uint8_t g_ctrl_tx_queue_storage[PWOS_QUEUE_STORAGE_BYTES(PWOS_CTRL_TX_QUEUE_LEN)]
    __attribute__((aligned(4)));
static uint8_t g_link_tx_queue_storage[PWOS_QUEUE_STORAGE_BYTES(PWOS_LINK_TX_QUEUE_LEN)]
    __attribute__((aligned(4)));

/* 队列句柄，初始化后非 NULL。 */
static QueueHandle_t g_link_rx_queue;
static QueueHandle_t g_mesh_rx_queue;
static QueueHandle_t g_service_rx_queue;
static QueueHandle_t g_ctrl_tx_queue;
static QueueHandle_t g_link_tx_queue;

/* 各队列累计丢包计数器，用于诊断。 */
static volatile uint32_t g_link_rx_drops;
static volatile uint32_t g_mesh_rx_drops;
static volatile uint32_t g_service_rx_drops;
static volatile uint32_t g_ctrl_tx_drops;
static volatile uint32_t g_link_tx_drops;

/*
 * 初始化所有静态队列。
 *
 * 使用 xQueueCreateStatic 创建队列，参数分别为：
 *   - uxQueueLength：队列深度。
 *   - uxItemSize：   每个元素大小，这里是 sizeof(pwos_frame_block_t *)。
 *   - pucQueueStorage：队列存储区指针。
 *   - pxStaticQueue：  静态控制块指针。
 *
 * @return 0 所有队列创建成功；-1 任一队列创建失败。
 */
int pwos_queues_init(void)
{
    g_link_rx_drops = 0u;
    g_mesh_rx_drops = 0u;
    g_service_rx_drops = 0u;
    g_ctrl_tx_drops = 0u;
    g_link_tx_drops = 0u;

    g_link_rx_queue = xQueueCreateStatic(
        PWOS_LINK_RX_QUEUE_LEN,
        sizeof(pwos_frame_block_t *),
        g_link_rx_queue_storage,
        &g_link_rx_queue_cb);
    g_mesh_rx_queue = xQueueCreateStatic(
        PWOS_MESH_RX_QUEUE_LEN,
        sizeof(pwos_frame_block_t *),
        g_mesh_rx_queue_storage,
        &g_mesh_rx_queue_cb);
    g_service_rx_queue = xQueueCreateStatic(
        PWOS_SERVICE_RX_QUEUE_LEN,
        sizeof(pwos_frame_block_t *),
        g_service_rx_queue_storage,
        &g_service_rx_queue_cb);
    g_ctrl_tx_queue = xQueueCreateStatic(
        PWOS_CTRL_TX_QUEUE_LEN,
        sizeof(pwos_frame_block_t *),
        g_ctrl_tx_queue_storage,
        &g_ctrl_tx_queue_cb);
    g_link_tx_queue = xQueueCreateStatic(
        PWOS_LINK_TX_QUEUE_LEN,
        sizeof(pwos_frame_block_t *),
        g_link_tx_queue_storage,
        &g_link_tx_queue_cb);

    if (g_link_rx_queue == NULL || g_mesh_rx_queue == NULL ||
        g_service_rx_queue == NULL ||
        g_ctrl_tx_queue == NULL || g_link_tx_queue == NULL) {
        return -1;
    }
    return 0;
}

/*
 * 从 ISR 向 link_rx 队列发送帧块指针。
 *
 * 这是 UART RX 中断唯一应该调用的入队接口。失败时增加 drop 计数并返回 pdFALSE，
 * 调用方（ISR）仍拥有 block，通常应立即释放回 frame_pool。
 *
 * @param block                    帧块指针。
 * @param higher_priority_task_woken 标准 FreeRTOS ISR 参数，可能触发调度。
 * @return                         pdPASS/pdFALSE。
 */
BaseType_t pwos_link_rx_send_from_isr(
    pwos_frame_block_t *block,
    BaseType_t *higher_priority_task_woken)
{
    BaseType_t ok;

    if (g_link_rx_queue == NULL || block == NULL) {
        ++g_link_rx_drops;
        return pdFALSE;
    }

    ok = xQueueSendFromISR(g_link_rx_queue, &block, higher_priority_task_woken);
    if (ok != pdPASS) {
        ++g_link_rx_drops;
    }
    return ok;
}

/*
 * link_rx_task 从 link_rx 队列接收帧块指针。
 *
 * @param block         输出帧块指针。
 * @param timeout_ticks 等待超时。
 * @return              pdPASS/pdFALSE。
 */
BaseType_t pwos_link_rx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks)
{
    if (g_link_rx_queue == NULL || block == NULL) {
        return pdFALSE;
    }
    return xQueueReceive(g_link_rx_queue, block, timeout_ticks);
}

/*
 * 向 mesh_rx 队列发送帧块指针。
 *
 * 由 link_rx_task 在过滤掉链路维护帧后调用。成功后 block 所有权转移给
 * mesh_ctrl_task。
 *
 * @param block         帧块指针。
 * @param timeout_ticks 等待超时；通常使用 0 避免在 link_rx_task 中阻塞。
 * @return              pdPASS/pdFALSE。
 */
BaseType_t pwos_mesh_rx_send(pwos_frame_block_t *block, TickType_t timeout_ticks)
{
    BaseType_t ok;

    if (g_mesh_rx_queue == NULL || block == NULL) {
        ++g_mesh_rx_drops;
        return pdFALSE;
    }

    ok = xQueueSend(g_mesh_rx_queue, &block, timeout_ticks);
    if (ok != pdPASS) {
        ++g_mesh_rx_drops;
    }
    return ok;
}

/*
 * mesh_ctrl_task 从 mesh_rx 队列接收帧块指针。
 *
 * @param block         输出帧块指针。
 * @param timeout_ticks 等待超时。
 * @return              pdPASS/pdFALSE。
 */
BaseType_t pwos_mesh_rx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks)
{
    if (g_mesh_rx_queue == NULL || block == NULL) {
        return pdFALSE;
    }
    return xQueueReceive(g_mesh_rx_queue, block, timeout_ticks);
}

BaseType_t pwos_service_rx_send(pwos_frame_block_t *block, TickType_t timeout_ticks)
{
    BaseType_t ok;

    if (g_service_rx_queue == NULL || block == NULL) {
        ++g_service_rx_drops;
        return pdFALSE;
    }

    ok = xQueueSend(g_service_rx_queue, &block, timeout_ticks);
    if (ok != pdPASS) {
        ++g_service_rx_drops;
    }
    return ok;
}

BaseType_t pwos_service_rx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks)
{
    if (g_service_rx_queue == NULL || block == NULL) {
        return pdFALSE;
    }
    return xQueueReceive(g_service_rx_queue, block, timeout_ticks);
}

BaseType_t pwos_ctrl_tx_send(pwos_frame_block_t *block, TickType_t timeout_ticks)
{
    BaseType_t ok;

    if (g_ctrl_tx_queue == NULL || block == NULL) {
        ++g_ctrl_tx_drops;
        return pdFALSE;
    }

    ok = xQueueSend(g_ctrl_tx_queue, &block, timeout_ticks);
    if (ok != pdPASS) {
        ++g_ctrl_tx_drops;
    }
    return ok;
}

BaseType_t pwos_ctrl_tx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks)
{
    if (g_ctrl_tx_queue == NULL || block == NULL) {
        return pdFALSE;
    }
    return xQueueReceive(g_ctrl_tx_queue, block, timeout_ticks);
}

/*
 * 向 link_tx 队列发送帧块指针。
 *
 * 由上层业务调用。成功后 block 所有权转移给 link_tx_task。
 *
 * @param block         帧块指针。
 * @param timeout_ticks 等待超时。
 * @return              pdPASS/pdFALSE。
 */
BaseType_t pwos_link_tx_send(pwos_frame_block_t *block, TickType_t timeout_ticks)
{
    BaseType_t ok;

    if (g_link_tx_queue == NULL || block == NULL) {
        ++g_link_tx_drops;
        return pdFALSE;
    }

    ok = xQueueSend(g_link_tx_queue, &block, timeout_ticks);
    if (ok != pdPASS) {
        ++g_link_tx_drops;
    }
    return ok;
}

/*
 * link_tx_task 从 link_tx 队列接收帧块指针。
 *
 * @param block         输出帧块指针。
 * @param timeout_ticks 等待超时。
 * @return              pdPASS/pdFALSE。
 */
BaseType_t pwos_link_tx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks)
{
    if (g_link_tx_queue == NULL || block == NULL) {
        return pdFALSE;
    }
    return xQueueReceive(g_link_tx_queue, block, timeout_ticks);
}

/*
 * 查询 link_rx 队列当前消息数。
 *
 * @return 当前队列中的帧块指针数量；未初始化返回 0。
 */
UBaseType_t pwos_link_rx_depth(void)
{
    return g_link_rx_queue == NULL ? 0u : uxQueueMessagesWaiting(g_link_rx_queue);
}

/*
 * 查询 mesh_rx 队列当前消息数。
 *
 * @return 当前队列中的帧块指针数量；未初始化返回 0。
 */
UBaseType_t pwos_mesh_rx_depth(void)
{
    return g_mesh_rx_queue == NULL ? 0u : uxQueueMessagesWaiting(g_mesh_rx_queue);
}

UBaseType_t pwos_service_rx_depth(void)
{
    return g_service_rx_queue == NULL ? 0u : uxQueueMessagesWaiting(g_service_rx_queue);
}

UBaseType_t pwos_ctrl_tx_depth(void)
{
    return g_ctrl_tx_queue == NULL ? 0u : uxQueueMessagesWaiting(g_ctrl_tx_queue);
}

/*
 * 查询 link_tx 队列当前消息数。
 *
 * @return 当前队列中的帧块指针数量；未初始化返回 0。
 */
UBaseType_t pwos_link_tx_depth(void)
{
    return g_link_tx_queue == NULL ? 0u : uxQueueMessagesWaiting(g_link_tx_queue);
}

/*
 * 查询 link_rx 队列累计丢包数。
 *
 * @return 累计丢包数。
 */
uint32_t pwos_link_rx_drop_count(void)
{
    return g_link_rx_drops;
}

/*
 * 查询 mesh_rx 队列累计丢包数。
 *
 * @return 累计丢包数。
 */
uint32_t pwos_mesh_rx_drop_count(void)
{
    return g_mesh_rx_drops;
}

uint32_t pwos_service_rx_drop_count(void)
{
    return g_service_rx_drops;
}

uint32_t pwos_ctrl_tx_drop_count(void)
{
    return g_ctrl_tx_drops;
}

/*
 * 查询 link_tx 队列累计丢包数。
 *
 * @return 累计丢包数。
 */
uint32_t pwos_link_tx_drop_count(void)
{
    return g_link_tx_drops;
}
