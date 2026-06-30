/*
 * pwos_tasks.c - PWOS RTOS 任务集实现
 *
 * 本文件实现 PWOS 固定任务及其创建、状态收集逻辑。
 *
 * 任务职责与数据流
 * ----------------
 *   UART RX 中断/DMA ──► uart_rx_task 重新 arm ReceiveToIdle DMA
 *        │
 *        ▼
 *   pwos_link_rx_send_from_isr() ──► link_rx_task
 *        │
 *        ▼
 *   pwos_port_manager_handle_rx()  链路维护帧（hello/heartbeat/error）
 *        │
 *        ▼（非链路维护帧）
 *   pwos_mesh_rx_send() ──► mesh_ctrl_task
 *        │
 *        ▼
 *   node_control 控制面/中继，或 service_task 处理本机 DATA_MINI9P/DATA_RPC
 *
 *   上层发送路径：
 *   pwos_link_tx_send() ──► link_tx_task ──► pwos_uart_dma_send()
 *
 * 栈与优先级设计
 * --------------
 *   - uart_rx 优先级最高（idle+5），只负责尽快重新 arm RX DMA。
 *   - link_rx / link_tx / port_mgr / mesh_ctrl 次高（idle+4），
 *     保证链路 I/O 和控制面响应及时。
 *   - service 次之（idle+3）。
 *   - app 再次之（idle+2）。
 *   - diag 最低（idle+1），避免监控影响业务。
 *
 * 所有任务栈空间都是静态数组，使用 xTaskCreateStatic 创建，适合没有 heap
 * 或 heap 受限的场景。
 */

#include "pwos_tasks.h"

#include <string.h>

#include "task.h"

#include "frame_pool.h"
#include "fault_control.h"
#include "node_control.h"
#include "port_manager.h"
#include "pwos_queues.h"
#include "service_runtime.h"
#include "uart_dma_port.h"

/*
 * 各任务栈大小（单位：字，32-bit）。
 *
 * 大小根据当前任务调用深度估算。如果后续加入复杂协议栈或大量局部变量，
 * 应通过 diag 任务输出的 stack_high_water_words 调整。
 */
#define PWOS_STACK_UART_RX_WORDS 256u
#define PWOS_STACK_LINK_RX_WORDS 512u
#define PWOS_STACK_LINK_TX_WORDS 384u
#define PWOS_STACK_PORT_MGR_WORDS 384u
#define PWOS_STACK_MESH_CTRL_WORDS 512u
#define PWOS_STACK_SERVICE_WORDS 768u
#define PWOS_STACK_APP_WORDS 384u
#define PWOS_STACK_DIAG_WORDS 384u

/*
 * 各任务优先级。
 *
 * 使用 tskIDLE_PRIORITY 作为基准，数值越大优先级越高。
 */
#define PWOS_PRIO_UART_RX (tskIDLE_PRIORITY + 5u)
#define PWOS_PRIO_LINK_RX (tskIDLE_PRIORITY + 4u)
#define PWOS_PRIO_LINK_TX (tskIDLE_PRIORITY + 4u)
#define PWOS_PRIO_PORT_MGR (tskIDLE_PRIORITY + 4u)
#define PWOS_PRIO_MESH_CTRL (tskIDLE_PRIORITY + 4u)
#define PWOS_PRIO_SERVICE (tskIDLE_PRIORITY + 3u)
#define PWOS_PRIO_APP (tskIDLE_PRIORITY + 2u)
#define PWOS_PRIO_DIAG (tskIDLE_PRIORITY + 1u)
#define PWOS_LINK_TX_GUARD_MS 2u

/*
 * 任务运行时记录。
 *
 * 在静态数组 g_task_runtime 中按 pwos_task_id_t 索引保存每个任务的
 * FreeRTOS 句柄和诊断信息。
 */
typedef struct {
    const char *name;              /* 任务名称，仅用于诊断显示 */
    TaskHandle_t handle;           /* FreeRTOS 任务句柄 */
    uint32_t heartbeat;            /* 主循环迭代次数 */
    uint32_t loop_count;           /* 与 heartbeat 同步，预留 */
    uint32_t last_tick;            /* 最近一次心跳 tick */
    uint32_t stack_high_water_words; /* 栈历史最低剩余字数 */
} pwos_task_runtime_t;

/* 静态 TCB 缓冲区，供 xTaskCreateStatic 使用。 */
static StaticTask_t g_link_rx_tcb;
static StaticTask_t g_uart_rx_tcb;
static StaticTask_t g_link_tx_tcb;
static StaticTask_t g_port_mgr_tcb;
static StaticTask_t g_mesh_ctrl_tcb;
static StaticTask_t g_service_tcb;
static StaticTask_t g_app_tcb;
static StaticTask_t g_diag_tcb;

/* 静态栈缓冲区。 */
static StackType_t g_link_rx_stack[PWOS_STACK_LINK_RX_WORDS];
static StackType_t g_uart_rx_stack[PWOS_STACK_UART_RX_WORDS];
static StackType_t g_link_tx_stack[PWOS_STACK_LINK_TX_WORDS];
static StackType_t g_port_mgr_stack[PWOS_STACK_PORT_MGR_WORDS];
static StackType_t g_mesh_ctrl_stack[PWOS_STACK_MESH_CTRL_WORDS];
static StackType_t g_service_stack[PWOS_STACK_SERVICE_WORDS];
static StackType_t g_app_stack[PWOS_STACK_APP_WORDS];
static StackType_t g_diag_stack[PWOS_STACK_DIAG_WORDS];

/*
 * 任务运行时表。
 *
 * 使用 C99  designated initializer 按 pwos_task_id_t 顺序初始化，
 * 保证 name 与 ID 一一对应。
 */
static pwos_task_runtime_t g_task_runtime[PWOS_TASK_COUNT] = {
    [PWOS_TASK_UART_RX] = { .name = "uart_rx" },
    [PWOS_TASK_LINK_RX] = { .name = "link_rx" },
    [PWOS_TASK_LINK_TX] = { .name = "link_tx" },
    [PWOS_TASK_PORT_MGR] = { .name = "port_mgr" },
    [PWOS_TASK_MESH_CTRL] = { .name = "mesh_ctrl" },
    [PWOS_TASK_SERVICE] = { .name = "service" },
    [PWOS_TASK_APP] = { .name = "app" },
    [PWOS_TASK_DIAG] = { .name = "diag" },
};

/*
 * 记录一次任务心跳。
 *
 * 每个任务在主循环末尾调用，更新心跳计数、循环次数、最后 tick，
 * 并通过 uxTaskGetStackHighWaterMark 采样栈使用余量。
 *
 * @param id 任务 ID。
 */
static void note_heartbeat(pwos_task_id_t id)
{
    if ((uint32_t)id >= (uint32_t)PWOS_TASK_COUNT) {
        return;
    }

    ++g_task_runtime[id].heartbeat;
    ++g_task_runtime[id].loop_count;
    g_task_runtime[id].last_tick = (uint32_t)xTaskGetTickCount();
    g_task_runtime[id].stack_high_water_words =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

/*
 * UART RX 恢复任务。
 *
 * 职责：
 *   - RX IDLE/TC/Error ISR 只标记 DMA 需要重新 arm，并通知本任务。
 *   - 本任务在普通任务上下文调用 HAL_UARTEx_ReceiveToIdle_DMA()。
 *
 * 这样可以避开在 HAL UART/DMA 回调栈内重启 DMA 的竞态，避免 HAL_BUSY
 * 或错误计数暴涨。10ms 超时是兜底轮询，防止调度器启动前的 pending 状态
 * 或异常通知丢失导致 RX 停住。
 */
static void uart_rx_task(void *argument)
{
    (void)argument;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10u));
        pwos_uart_dma_poll_recover();
        note_heartbeat(PWOS_TASK_UART_RX);
    }
}

/*
 * 链路接收任务。
 *
 * 职责：
 *   - 从 link_rx 队列等待帧块（ISR 收到完整帧后投递）。
 *   - 先把帧交给 port_manager 处理链路维护帧（hello/heartbeat/error）。
 *   - 如果 port_manager 不处理（返回 0），则投递到 mesh_rx 队列。
 *   - 任何路径上如果无法继续传递，必须释放帧块，避免泄漏。
 */
static void link_rx_task(void *argument)
{
    (void)argument;

    for (;;) {
        pwos_frame_block_t *block = NULL;

        if (pwos_link_rx_receive(&block, pdMS_TO_TICKS(100u)) == pdPASS) {
            /*
             * 链路维护帧先由 port_manager 拥有；其它帧才进入 mesh 控制入口。
             * port_manager_handle_rx 返回非 0 表示它已处理并接管/释放 block。
             */
            if (pwos_fault_control_drop_rx(block->port_id) != 0) {
                pwos_frame_pool_free(block);
            } else if (pwos_port_manager_handle_rx(block) != 0) {
                pwos_frame_pool_free(block);
            } else if (pwos_mesh_rx_send(block, 0u) != pdPASS) {
                /* mesh 队列满或初始化失败，必须释放 block。 */
                pwos_frame_pool_free(block);
            }
        }

        note_heartbeat(PWOS_TASK_LINK_RX);
    }
}

/*
 * 链路发送任务。
 *
 * 职责：
 *   - 优先从 ctrl_tx 队列发送控制面帧。
 *   - ctrl_tx 为空时再从 link_tx 队列等待普通数据面帧。
 *   - 调用 UART DMA 发送。
 *   - 等待 DMA 发送完成后释放帧块，并保留接收端重新 arm DMA 的帧间隔。
 */
static void link_tx_task(void *argument)
{
    (void)argument;

    for (;;) {
        pwos_frame_block_t *block = NULL;

        if (pwos_ctrl_tx_receive(&block, 0u) != pdPASS) {
            (void)pwos_link_tx_receive(&block, pdMS_TO_TICKS(100u));
        }

        if (block != NULL) {
            /* TX 队列拥有完整 link frame，发送完成或失败后归还块。 */
            if (pwos_fault_control_before_tx(
                    block->port_id, block->data, block->len) == 0) {
                (void)pwos_uart_dma_send(
                    block->port_id, block->data, block->len, 50u);
            }
            pwos_frame_pool_free(block);
            /*
             * 对端 RX 是 DMA_NORMAL + ReceiveToIdle。两帧紧邻会让下一帧撞上
             * IDLE 回调后的 DMA rearm 空窗，因此发送完成后主动留出保护时间。
             */
            vTaskDelay(pdMS_TO_TICKS(PWOS_LINK_TX_GUARD_MS));
        }

        note_heartbeat(PWOS_TASK_LINK_TX);
    }
}

/*
 * 端口管理任务。
 *
 * 职责：
 *   - 周期调用 pwos_port_manager_tick() 驱动端口发现状态机。
 *
 * 该任务以 100ms 为周期运行，不处理具体帧，只维护端口状态。
 */
static void port_mgr_task(void *argument)
{
    (void)argument;

    for (;;) {
        /*
         * port_manager 拥有端口发现 FSM；UART RX 恢复由 uart_rx_task 处理。
         */
        pwos_port_manager_tick();
        pwos_node_control_tick();
        pwos_fault_control_poll();
        note_heartbeat(PWOS_TASK_PORT_MGR);
        vTaskDelay(pdMS_TO_TICKS(100u));
    }
}

/*
 * Mesh 控制任务。
 *
 * 职责：
 *   - 从 mesh_rx 队列接收链路层已解码的帧。
 *   - 控制面和需要中继的数据面交给 node_control。
 *   - 本机 DATA_MINI9P/DATA_RPC 请求转入 service_rx，由 service_task 串行处理。
 */
static void mesh_ctrl_task(void *argument)
{
    (void)argument;

    for (;;) {
        pwos_frame_block_t *block = NULL;

        if (pwos_mesh_rx_receive(&block, pdMS_TO_TICKS(100u)) == pdPASS) {
            /*
             * 控制面和中继数据面先由 node_control 处理。
             * node_control 返回 0 时，说明这是本机 service 层关心的数据面帧。
             */
            if (pwos_node_control_handle_rx(block) != 0) {
                pwos_frame_pool_free(block);
            } else if (pwos_service_runtime_accepts(block) != 0) {
                if (pwos_service_rx_send(block, 0u) != pdPASS) {
                    pwos_frame_pool_free(block);
                }
            } else {
                pwos_frame_pool_free(block);
            }
        }

        note_heartbeat(PWOS_TASK_MESH_CTRL);
    }
}

/*
 * 系统服务任务。
 *
 * 处理本机 mini9P/RPC 请求，并推进延期 RPC。10ms 的接收超时限定了
 * deadline 响应的调度粒度，同时不会阻塞链路与控制面任务。
 */
static void service_task(void *argument)
{
    (void)argument;

    for (;;) {
        pwos_frame_block_t *block = NULL;

        if (pwos_service_rx_receive(&block, pdMS_TO_TICKS(10u)) == pdPASS) {
            pwos_service_runtime_process(block);
            pwos_frame_pool_free(block);
        }
        pwos_service_runtime_poll();

        note_heartbeat(PWOS_TASK_SERVICE);
    }
}

/*
 * 应用任务（占位）。
 *
 * 当前仅用于保持心跳，周期 250ms。后续可加入用户业务逻辑。
 */
static void app_task(void *argument)
{
    (void)argument;

    for (;;) {
        note_heartbeat(PWOS_TASK_APP);
        vTaskDelay(pdMS_TO_TICKS(250u));
    }
}

/*
 * 诊断任务。
 *
 * 职责：
 *   - 周期读取关键运行时计数（帧池余量、队列深度等）。
 *   - 这些调用会把相关对象链接进最终镜像，并便于调试器观察。
 *
 * 以 1000ms 为周期运行，优先级最低，避免影响业务。
 */
static void diag_task(void *argument)
{
    (void)argument;

    for (;;) {
        /*
         * 读这些计数会强制把关键对象链接进来，便于调试器观察。
         */
        (void)pwos_frame_pool_free_count();
        (void)pwos_link_rx_depth();
        (void)pwos_mesh_rx_depth();
        (void)pwos_service_rx_depth();
        (void)pwos_ctrl_tx_depth();
        (void)pwos_link_tx_depth();
        {
            pwos_node_control_snapshot_t node;
            pwos_node_control_get_snapshot(&node);
        }
        {
            pwos_service_runtime_stats_t service;
            pwos_service_runtime_get_stats(&service);
        }
        note_heartbeat(PWOS_TASK_DIAG);
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
}

/*
 * 创建一个静态任务并记录其句柄。
 *
 * @param id          任务 ID，用于写入 g_task_runtime。
 * @param entry       任务入口函数。
 * @param name        任务名称。
 * @param stack_words 栈大小（字）。
 * @param priority    FreeRTOS 优先级。
 * @param stack       静态栈缓冲区。
 * @param tcb         静态 TCB 缓冲区。
 * @return            0 成功；-1 失败。
 */
static int create_one_task(
    pwos_task_id_t id,
    TaskFunction_t entry,
    const char *name,
    uint32_t stack_words,
    UBaseType_t priority,
    StackType_t *stack,
    StaticTask_t *tcb)
{
    TaskHandle_t handle;

    handle = xTaskCreateStatic(entry, name, stack_words, NULL, priority, stack, tcb);
    g_task_runtime[id].handle = handle;
    return handle == NULL ? -1 : 0;
}

/*
 * 创建并启动所有 M2 任务。
 *
 * 实现细节：
 *   - 先清零各静态 TCB（虽然不是严格必须，但可以避免未初始化字段的依赖）。
 *   - 依次创建 8 个任务，任一失败都会使最终返回 -1。
 *   - 任务创建后处于就绪态，调度器启动后立即按优先级运行。
 *
 * @return 0 成功；-1 至少一个任务创建失败。
 */
int pwos_tasks_start(void)
{
    int rc = 0;

    pwos_fault_control_init();
    memset(&g_uart_rx_tcb, 0, sizeof(g_uart_rx_tcb));
    memset(&g_link_rx_tcb, 0, sizeof(g_link_rx_tcb));
    memset(&g_link_tx_tcb, 0, sizeof(g_link_tx_tcb));
    memset(&g_port_mgr_tcb, 0, sizeof(g_port_mgr_tcb));
    memset(&g_mesh_ctrl_tcb, 0, sizeof(g_mesh_ctrl_tcb));
    memset(&g_service_tcb, 0, sizeof(g_service_tcb));
    memset(&g_app_tcb, 0, sizeof(g_app_tcb));
    memset(&g_diag_tcb, 0, sizeof(g_diag_tcb));

    rc |= create_one_task(PWOS_TASK_UART_RX, uart_rx_task, "uart_rx",
        PWOS_STACK_UART_RX_WORDS, PWOS_PRIO_UART_RX, g_uart_rx_stack, &g_uart_rx_tcb);
    rc |= create_one_task(PWOS_TASK_LINK_RX, link_rx_task, "link_rx",
        PWOS_STACK_LINK_RX_WORDS, PWOS_PRIO_LINK_RX, g_link_rx_stack, &g_link_rx_tcb);
    rc |= create_one_task(PWOS_TASK_LINK_TX, link_tx_task, "link_tx",
        PWOS_STACK_LINK_TX_WORDS, PWOS_PRIO_LINK_TX, g_link_tx_stack, &g_link_tx_tcb);
    rc |= create_one_task(PWOS_TASK_PORT_MGR, port_mgr_task, "port_mgr",
        PWOS_STACK_PORT_MGR_WORDS, PWOS_PRIO_PORT_MGR, g_port_mgr_stack, &g_port_mgr_tcb);
    rc |= create_one_task(PWOS_TASK_MESH_CTRL, mesh_ctrl_task, "mesh_ctrl",
        PWOS_STACK_MESH_CTRL_WORDS, PWOS_PRIO_MESH_CTRL, g_mesh_ctrl_stack, &g_mesh_ctrl_tcb);
    rc |= create_one_task(PWOS_TASK_SERVICE, service_task, "service",
        PWOS_STACK_SERVICE_WORDS, PWOS_PRIO_SERVICE, g_service_stack, &g_service_tcb);
    rc |= create_one_task(PWOS_TASK_APP, app_task, "app",
        PWOS_STACK_APP_WORDS, PWOS_PRIO_APP, g_app_stack, &g_app_tcb);
    rc |= create_one_task(PWOS_TASK_DIAG, diag_task, "diag",
        PWOS_STACK_DIAG_WORDS, PWOS_PRIO_DIAG, g_diag_stack, &g_diag_tcb);

    if (g_task_runtime[PWOS_TASK_UART_RX].handle != NULL) {
        pwos_uart_dma_set_recover_task(g_task_runtime[PWOS_TASK_UART_RX].handle);
    }

    return rc == 0 ? 0 : -1;
}

/*
 * 获取任务状态快照。
 *
 * 把内部运行时表中的诊断信息复制到调用方缓冲区。不会修改内部状态。
 *
 * @param out_status    输出缓冲区。
 * @param out_capacity  输出缓冲区条目数。
 * @return              实际写入条目数。
 */
size_t pwos_tasks_get_status(pwos_task_status_t *out_status, size_t out_capacity)
{
    size_t i;
    size_t count;

    if (out_status == NULL || out_capacity == 0u) {
        return 0u;
    }

    count = out_capacity < (size_t)PWOS_TASK_COUNT ? out_capacity : (size_t)PWOS_TASK_COUNT;
    for (i = 0u; i < count; ++i) {
        out_status[i].name = g_task_runtime[i].name;
        out_status[i].heartbeat = g_task_runtime[i].heartbeat;
        out_status[i].loop_count = g_task_runtime[i].loop_count;
        out_status[i].last_tick = g_task_runtime[i].last_tick;
        out_status[i].stack_high_water_words = g_task_runtime[i].stack_high_water_words;
        out_status[i].priority = g_task_runtime[i].handle == NULL ?
            0u : (uint32_t)uxTaskPriorityGet(g_task_runtime[i].handle);
    }

    return count;
}
