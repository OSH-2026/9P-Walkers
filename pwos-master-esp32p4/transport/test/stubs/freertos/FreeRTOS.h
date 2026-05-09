#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H

typedef unsigned int TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif