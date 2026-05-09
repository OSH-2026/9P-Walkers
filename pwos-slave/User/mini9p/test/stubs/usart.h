#ifndef TEST_USART_H
#define TEST_USART_H

#include <stdint.h>

typedef enum {
    HAL_OK = 0,
    HAL_ERROR = 1,
    HAL_BUSY = 2,
    HAL_TIMEOUT = 3,
} HAL_StatusTypeDef;

typedef struct {
    void *Instance;
    void *test_state;
} UART_HandleTypeDef;

extern UART_HandleTypeDef huart2;

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *huart,
                                    const uint8_t *pData,
                                    uint16_t Size,
                                    uint32_t Timeout);
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *huart,
                                   uint8_t *pData,
                                   uint16_t Size,
                                   uint32_t Timeout);
void fake_uart_flush(UART_HandleTypeDef *huart);

#define __HAL_UART_FLUSH_DRREGISTER(__HANDLE__) fake_uart_flush((__HANDLE__))

#endif