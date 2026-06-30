/*
 * pwos_queues.h - PWOS M2 队列封装接口
 *
 * 本文件封装 M2/M4 使用的 4 条 FreeRTOS 静态队列：
 *   - link_rx：  ISR/驱动 -> link_rx_task，存放从 UART 接收到的帧块指针。
 *   - mesh_rx：  link_rx_task -> mesh_ctrl_task，存放待进入 mesh 层的帧块指针。
 *   - service_rx：mesh_ctrl_task -> service_task，存放本机 DATA_MINI9P 请求。
 *   - ctrl_tx：  mesh 控制面 -> link_tx_task，高优先级发送队列。
 *   - link_tx：  上层 -> link_tx_task，存放待发送的帧块指针。
 *
 * 设计要点
 * --------
 *   - 队列元素类型为 pwos_frame_block_t *（指针），不是帧本身。
 *     实际帧数据存储在 frame_pool 管理的块中，队列只传递所有权。
 *   - 所有队列都是静态创建（xQueueCreateStatic），无需动态内存。
 *   - 队列深度固定为 16，可通过下面宏调整。
 *   - 提供 drop 计数和深度查询，用于诊断和流控。
 *
 * 所有权规则
 * ----------
 *   - send 成功后，block 所有权转移给接收方。
 *   - send 失败时，调用方仍拥有 block，必须负责释放。
 *   - receive 成功后，接收方拥有 block，处理完后必须释放。
 */

#ifndef PWOS_QUEUES_H
#define PWOS_QUEUES_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "frame_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 队列深度。
 *
 * 每个队列最多容纳 16 个 pwos_frame_block_t 指针。
 * 如果链路突发较高，可以适当增加 link_rx_queue_len。
 */
#define PWOS_LINK_RX_QUEUE_LEN 16u
#define PWOS_MESH_RX_QUEUE_LEN 16u
#define PWOS_SERVICE_RX_QUEUE_LEN 8u
#define PWOS_CTRL_TX_QUEUE_LEN 16u
#define PWOS_LINK_TX_QUEUE_LEN 16u

/*
 * 初始化所有静态队列。
 *
 * 必须在 pwos_tasks_start() 之前调用一次，因为任务创建后会立即开始等待队列。
 *
 * @return 0 成功；-1 任一队列创建失败。
 */
int pwos_queues_init(void);

/*
 * 从 ISR 向 link_rx 队列发送帧块指针。
 *
 * 这是 UART RX 完成中断/DMA 半传输/传输完成中断使用的接口。
 *
 * @param block                    待发送的帧块指针。
 * @param higher_priority_task_woken FreeRTOS 标准输出参数，用于中断返回前
 *                                   决定是否触发 PendSV 进行上下文切换。
 * @return                         pdPASS 成功；pdFALSE 失败（队列满或参数错误）。
 *                                 失败时调用方仍拥有 block。
 */
BaseType_t pwos_link_rx_send_from_isr(
    pwos_frame_block_t *block,
    BaseType_t *higher_priority_task_woken);

/*
 * link_rx_task 从 link_rx 队列接收帧块指针。
 *
 * @param block         输出参数，接收到的帧块指针。
 * @param timeout_ticks 等待 tick 数，可用 pdMS_TO_TICKS 转换。
 * @return              pdPASS 成功；pdFALSE 超时或参数错误。
 */
BaseType_t pwos_link_rx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks);

/*
 * 向 mesh_rx 队列发送帧块指针。
 *
 * 调用上下文：任务（link_rx_task）。
 *
 * @param block         待发送的帧块指针。
 * @param timeout_ticks 等待 tick 数；0 表示不等待。
 * @return              pdPASS 成功；pdFALSE 失败，调用方仍拥有 block。
 */
BaseType_t pwos_mesh_rx_send(pwos_frame_block_t *block, TickType_t timeout_ticks);

/*
 * mesh_ctrl_task 从 mesh_rx 队列接收帧块指针。
 *
 * @param block         输出参数，接收到的帧块指针。
 * @param timeout_ticks 等待 tick 数。
 * @return              pdPASS 成功；pdFALSE 超时或参数错误。
 */
BaseType_t pwos_mesh_rx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks);

/*
 * 向 service 队列发送本机数据面请求。
 *
 * 调用上下文：mesh_ctrl_task。成功后 block 所有权转移给 service_task。
 */
BaseType_t pwos_service_rx_send(pwos_frame_block_t *block, TickType_t timeout_ticks);

/*
 * service_task 从 service 队列接收本机数据面请求。
 */
BaseType_t pwos_service_rx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks);

/*
 * 向控制面高优先级 TX 队列发送帧块指针。
 *
 * 调用上下文：任务。成功后 block 所有权转移给 link_tx_task。
 */
BaseType_t pwos_ctrl_tx_send(pwos_frame_block_t *block, TickType_t timeout_ticks);

/*
 * link_tx_task 从控制面高优先级 TX 队列接收帧块指针。
 */
BaseType_t pwos_ctrl_tx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks);

/*
 * 向 link_tx 队列发送帧块指针。
 *
 * 调用上下文：任务。成功后 block 所有权转移给 link_tx_task。
 *
 * @param block         待发送的帧块指针。
 * @param timeout_ticks 等待 tick 数；0 表示不等待。
 * @return              pdPASS 成功；pdFALSE 失败，调用方仍拥有 block。
 */
BaseType_t pwos_link_tx_send(pwos_frame_block_t *block, TickType_t timeout_ticks);

/*
 * link_tx_task 从 link_tx 队列接收帧块指针。
 *
 * @param block         输出参数，接收到的帧块指针。
 * @param timeout_ticks 等待 tick 数。
 * @return              pdPASS 成功；pdFALSE 超时或参数错误。
 */
BaseType_t pwos_link_tx_receive(pwos_frame_block_t **block, TickType_t timeout_ticks);

/*
 * 查询各队列当前消息数。
 *
 * @return 队列中等待处理的帧块指针数量；队列未初始化时返回 0。
 */
UBaseType_t pwos_link_rx_depth(void);
UBaseType_t pwos_mesh_rx_depth(void);
UBaseType_t pwos_service_rx_depth(void);
UBaseType_t pwos_ctrl_tx_depth(void);
UBaseType_t pwos_link_tx_depth(void);

/*
 * 查询各队列累计丢包数。
 *
 * 每次 send 失败（队列满或参数错误）时对应计数器加 1。
 *
 * @return 累计丢包数。
 */
uint32_t pwos_link_rx_drop_count(void);
uint32_t pwos_mesh_rx_drop_count(void);
uint32_t pwos_service_rx_drop_count(void);
uint32_t pwos_ctrl_tx_drop_count(void);
uint32_t pwos_link_tx_drop_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_QUEUES_H */
