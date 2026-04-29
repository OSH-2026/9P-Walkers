/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "sdio.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "fs_selftest.h"
#include "mini9p_server.h"
#include "vofa_firewater.h"
#include <stdio.h>
#include <string.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
HAL_SD_CardInfoTypeDef SDCardInfo;
FS_SelfTestReport g_fs_report;
static struct m9p_server g_m9p_server;
static uint8_t g_m9p_rx_buffer[M9P_SERVER_FRAME_CAP];
static size_t g_m9p_rx_len;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void UART_ReportEarlyBoot(void);
static void FS_ReportBootStatus(void);
static void M9P_ResetRxState(void);
static void M9P_ResyncRxState(void);
static void M9P_ProcessRxByte(uint8_t byte);
static void M9P_PollUart(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void UART_ReportEarlyBoot(void)
{
  (void)vofa_firewater_send_text("boot: uart2 ok");
}

static void FS_ReportBootStatus(void)
{
  char message[128];
  int pass;

  pass = (g_fs_report.init_status == 0) &&
         (g_fs_report.mkdir_status == 0) &&
         (g_fs_report.write_status == 0) &&
         (g_fs_report.stat_status == 0) &&
         (g_fs_report.read_status == 0) &&
         (g_fs_report.dir_status == 0) &&
         (g_fs_report.compare_status == 0);

  if (snprintf(message, sizeof(message),
               "boot status=%s init=%ld file_size=%lu read=%lu write=%lu",
               pass ? "PASS" : "FAIL",
               (long)g_fs_report.init_status,
               (unsigned long)g_fs_report.file_size,
               (unsigned long)g_fs_report.bytes_read,
               (unsigned long)g_fs_report.bytes_written) > 0) {
    (void)vofa_firewater_send_text(message);
  }
}

static void M9P_ResetRxState(void)
{
  g_m9p_rx_len = 0u;
}

static void M9P_ResyncRxState(void)
{
  size_t offset;

  for (offset = 1u; offset + 1u < g_m9p_rx_len; ++offset)
  {
    if ((g_m9p_rx_buffer[offset] == (uint8_t)'9') &&
        (g_m9p_rx_buffer[offset + 1u] == (uint8_t)'P'))
    {
      memmove(g_m9p_rx_buffer, g_m9p_rx_buffer + offset, g_m9p_rx_len - offset);
      g_m9p_rx_len -= offset;
      return;
    }
  }

  if ((g_m9p_rx_len > 0u) && (g_m9p_rx_buffer[g_m9p_rx_len - 1u] == (uint8_t)'9'))
  {
    g_m9p_rx_buffer[0] = (uint8_t)'9';
    g_m9p_rx_len = 1u;
    return;
  }

  M9P_ResetRxState();
}

static void M9P_ProcessRxByte(uint8_t byte)
{
  if (g_m9p_rx_len == 0u)
  {
    if (byte == (uint8_t)'9')
    {
      g_m9p_rx_buffer[g_m9p_rx_len++] = byte;
    }
    return;
  }

  if (g_m9p_rx_len == 1u)
  {
    if (byte == (uint8_t)'P')
    {
      g_m9p_rx_buffer[g_m9p_rx_len++] = byte;
      return;
    }
    g_m9p_rx_buffer[0] = byte;
    g_m9p_rx_len = (byte == (uint8_t)'9') ? 1u : 0u;
    return;
  }

  if (g_m9p_rx_len >= sizeof(g_m9p_rx_buffer))
  {
    M9P_ResyncRxState();
  }
  if (g_m9p_rx_len >= sizeof(g_m9p_rx_buffer))
  {
    M9P_ResetRxState();
  }

  g_m9p_rx_buffer[g_m9p_rx_len++] = byte;
  if (g_m9p_rx_len >= 4u)
  {
    const uint16_t frame_len_field =
        (uint16_t)g_m9p_rx_buffer[2] |
        (uint16_t)((uint16_t)g_m9p_rx_buffer[3] << 8);
    const size_t expected_len = (size_t)frame_len_field + 6u;

    if ((expected_len < M9P_FRAME_OVERHEAD) || (expected_len > sizeof(g_m9p_rx_buffer)))
    {
      M9P_ResyncRxState();
      return;
    }
    if (g_m9p_rx_len < expected_len)
    {
      return;
    }
    if (g_m9p_rx_len == expected_len)
    {
      uint8_t response_frame[M9P_SERVER_FRAME_CAP];
      size_t response_len = 0u;

      if (m9p_server_handle_frame(&g_m9p_server,
                                  g_m9p_rx_buffer,
                                  g_m9p_rx_len,
                                  response_frame,
                                  sizeof(response_frame),
                                  &response_len))
      {
        if (response_len > 0u)
        {
          (void)HAL_UART_Transmit(&huart2, response_frame, (uint16_t)response_len, 20u);
        }
      }
      M9P_ResetRxState();
      return;
    }

    M9P_ResyncRxState();
  }
}

static void M9P_PollUart(void)
{
  uint8_t byte;

  while (HAL_UART_Receive(&huart2, &byte, 1u, 0u) == HAL_OK)
  {
    M9P_ProcessRxByte(byte);
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_SDIO_SD_Init();
  /* USER CODE BEGIN 2 */
  UART_ReportEarlyBoot();
  (void)vofa_firewater_send_text("boot: sdio ok");
  if (HAL_SD_GetCardInfo(&hsd, &SDCardInfo) == HAL_OK)
  {
    (void)vofa_firewater_send_text("boot: sd cardinfo ok");
  }
  else
  {
    (void)vofa_firewater_send_text("boot: sd cardinfo fail");
  }
  (void)fs_selftest_run(&g_fs_report);
  FS_ReportBootStatus();
  m9p_server_init(&g_m9p_server);
  M9P_ResetRxState();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  {
    uint32_t last_report_tick = HAL_GetTick();

  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    M9P_PollUart();
    if ((HAL_GetTick() - last_report_tick) >= 1000u)
    {
      (void)vofa_firewater_send_fs_report(&g_fs_report, HAL_GetTick());
      last_report_tick = HAL_GetTick();
    }
  }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  {
    const uint8_t error_message[] = "error: entered Error_Handler\n";
    (void)HAL_UART_Transmit(&huart2, error_message,
                            sizeof(error_message) - 1U, 100U);
  }
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
