#ifndef TEST_SEMPHR_H
#define TEST_SEMPHR_H

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);
int xSemaphoreGive(SemaphoreHandle_t xSemaphore);
void vSemaphoreDelete(SemaphoreHandle_t xSemaphore);

#endif