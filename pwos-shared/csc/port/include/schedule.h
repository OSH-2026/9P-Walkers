#ifndef PWOS_CSC_SCHEDULE_H
#define PWOS_CSC_SCHEDULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * lttit scp 只需要 rtos_now_time() 作为毫秒时间源。
 * 这里不引入 lttit 自研 RTOS 调度器。
 */
uint32_t rtos_now_time(void);

#ifdef __cplusplus
}
#endif

#endif
