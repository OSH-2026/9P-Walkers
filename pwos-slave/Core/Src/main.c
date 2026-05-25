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
#ifdef PWOS_ENABLE_MINI9P_SERIAL
#include "mini9p_service.h"
#else
#include "fs_selftest.h"
#include "vofa_firewater.h"
#include <stdio.h>
#endif

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
#ifndef PWOS_ENABLE_MINI9P_SERIAL
HAL_SD_CardInfoTypeDef SDCardInfo;
FS_SelfTestReport g_fs_report;
#endif
#if defined(PWOS_ENABLE_MINI9P_SERIAL) && defined(PWOS_BOARD_ZGT6)
static uint32_t g_led_last_toggle_ms;
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
#ifndef PWOS_ENABLE_MINI9P_SERIAL
static void FS_ReportBootStatus(void);
#endif

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#ifndef PWOS_ENABLE_MINI9P_SERIAL
static void FS_ReportBootStatus(void) {
  char message[128];
  int pass;

  pass = (g_fs_report.init_status == 0) && (g_fs_report.mkdir_status == 0) &&
         (g_fs_report.write_status == 0) && (g_fs_report.stat_status == 0) &&
         (g_fs_report.read_status == 0) && (g_fs_report.dir_status == 0) &&
         (g_fs_report.compare_status == 0) &&
         (g_fs_report.fixture_status == 0) &&
         (g_fs_report.report_status == 0);

  if (snprintf(message, sizeof(message),
               "boot status=%s init=%ld fixture=%ld file_size=%lu read=%lu write=%lu",
               pass ? "PASS" : "FAIL", (long)g_fs_report.init_status,
               (long)g_fs_report.fixture_status,
               (unsigned long)g_fs_report.file_size,
               (unsigned long)g_fs_report.bytes_read,
               (unsigned long)g_fs_report.bytes_written) > 0) {
    (void)vofa_firewater_send_text(message);
  }
}
#endif

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
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
#ifdef PWOS_BOARD_ZGT6
  MX_GPIO_PF9_LED_Init();
#ifdef PWOS_ENABLE_MINI9P_SERIAL
  HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_SET);
#endif
#endif
#ifdef PWOS_ENABLE_MINI9P_SERIAL
  if (mini9p_service_init() != 0) {
    Error_Handler();
  }
#else
  (void)vofa_firewater_send_text("boot: sdio ok");
  if (HAL_SD_GetCardInfo(&hsd, &SDCardInfo) == HAL_OK) {
    (void)vofa_firewater_send_text("boot: sd cardinfo ok");
  } else {
    (void)vofa_firewater_send_text("boot: sd cardinfo fail");
  }
  (void)fs_selftest_run(&g_fs_report);
  FS_ReportBootStatus();
#endif
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
#ifdef PWOS_ENABLE_MINI9P_SERIAL
#ifdef PWOS_BOARD_ZGT6
    {
      uint32_t now = HAL_GetTick();
      if ((uint32_t)(now - g_led_last_toggle_ms) >= 100U) {
        HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9);  // PF9 接绿色 LED
        g_led_last_toggle_ms = now;
      }
    }
#endif
    (void)mini9p_service_poll_once();
#else
    (void)vofa_firewater_send_fs_report(&g_fs_report, HAL_GetTick());
    HAL_Delay(1000);
#endif
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
#if defined(PWOS_ENABLE_MINI9P_SERIAL) && defined(PWOS_BOARD_ZGT6)
  while (1) {
    HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9);
    HAL_Delay(50);
  }
#endif
#ifndef PWOS_ENABLE_MINI9P_SERIAL
  /* User can add his own implementation to report the HAL error return state */
  {
    const uint8_t error_message[] = "error: entered Error_Handler\n";
    (void)HAL_UART_Transmit(&huart2, error_message, sizeof(error_message) - 1U,
                            100U);
  }
#endif
  __disable_irq();
  while (1) {
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
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
