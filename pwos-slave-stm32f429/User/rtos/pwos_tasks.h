/*
 * pwos_tasks.h - PWOS M2 任务集管理接口
 *
 * 本文件定义 PWOS M2（STM32F407 从机）运行时的任务 ID、任务状态快照，
 * 以及创建/查询任务的 API。
 *
 * 任务概览
 * --------
 * M2 当前包含 8 个固定任务，按优先级从高到低排列：
 *   - uart_rx：   在任务态重新 arm UART ReceiveToIdle DMA。
 *   - link_rx：   从 UART DMA 接收链路帧，交给 port_manager 或 mesh 控制面。
 *   - link_tx：   从发送队列取帧，通过 UART DMA 发出。
 *   - port_mgr：  维护端口发现状态机，定期 poll UART 恢复。
 *   - mesh_ctrl： 处理 mesh 控制面和中继数据面帧。
 *   - service：   串行处理本机 DATA_MINI9P 请求。
 *   - app：       应用层任务，周期最慢。
 *   - diag：      诊断任务，定期读取队列深度、帧池余量等。
 *
 * 所有任务都使用 FreeRTOS 静态任务 API（xTaskCreateStatic）创建，栈和 TCB
 * 都是静态分配的，避免动态内存。
 *
 * 初始化顺序
 * ----------
 * 1. 调用 pwos_queues_init() 创建队列。
 * 2. 调用 pwos_tasks_start() 创建任务。
 * 3. 调用 vTaskStartScheduler() 启动调度器。
 */

#ifndef PWOS_TASKS_H
#define PWOS_TASKS_H

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 任务 ID 枚举。
 *
 * 每个 ID 对应一个固定任务，也是 g_task_runtime 数组的索引。
 * PWOS_TASK_COUNT 必须放在最后，用于数组大小和循环边界。
 */
typedef enum {
    PWOS_TASK_UART_RX = 0,   /* UART RX DMA 重新 arm 任务 */
    PWOS_TASK_LINK_RX,       /* 链路接收任务 */
    PWOS_TASK_LINK_TX,       /* 链路发送任务 */
    PWOS_TASK_PORT_MGR,      /* 端口管理任务 */
    PWOS_TASK_MESH_CTRL,     /* mesh 控制/数据处理任务 */
    PWOS_TASK_SERVICE,       /* 系统服务任务 */
    PWOS_TASK_COMPUTE,       /* 低优先级计算 worker */
    PWOS_TASK_APP,           /* 应用任务 */
    PWOS_TASK_DIAG,          /* 诊断/监控任务 */
    PWOS_TASK_COUNT          /* 任务总数，不是有效任务 ID */
} pwos_task_id_t;

/*
 * 任务状态快照。
 *
 * 用于诊断任务或外部调试工具获取各任务运行状态。
 */
typedef struct {
    const char *name;              /* 任务名称 */
    uint32_t heartbeat;            /* 任务主循环执行次数 */
    uint32_t loop_count;           /* 与 heartbeat 同步递增，预留扩展 */
    uint32_t last_tick;            /* 最近一次心跳时的 FreeRTOS tick */
    uint32_t stack_high_water_words; /* 栈使用历史最低剩余字数（越大越安全） */
    uint32_t priority;             /* 当前 FreeRTOS 任务优先级 */
} pwos_task_status_t;

/*
 * 创建并启动所有 M2 任务。
 *
 * 该函数必须在 FreeRTOS 调度器启动前调用，且必须在 pwos_queues_init() 之后调用，
 * 因为各任务会立即开始等待队列消息。
 *
 * @return 0 表示所有任务创建成功；负数表示至少一个任务创建失败。
 */
int pwos_tasks_start(void);

/*
 * 获取任务状态快照。
 *
 * 把内部维护的任务运行状态复制到调用方提供的缓冲区中。
 *
 * @param out_status    输出缓冲区。
 * @param out_capacity  输出缓冲区可容纳的条目数。
 * @return              实际写入的条目数，不会超过 PWOS_TASK_COUNT。
 */
size_t pwos_tasks_get_status(
    pwos_task_status_t *out_status,
    size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_TASKS_H */
