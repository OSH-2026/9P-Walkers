/**
 * @file bsp_sdram.c
 * @brief SDRAM chip initialization sequence for the IS42S16400J on the
 *        STM32F429I-DISCO board.
 *
 * CubeMX leaves FMC configured but never sends the SDRAM device commands.
 * Sequence follows the standard IS42S16400J power-on init:
 *   1. Clock enable
 *   2. >= 100us delay (we use 1ms)
 *   3. Precharge all banks
 *   4. Auto-refresh x 8
 *   5. Load mode register (BL=1, sequential, CL=3, single write)
 *   6. Program refresh counter
 *
 * Refresh math:
 *   - System clock = 72 MHz (HCLK)
 *   - SDRAM clock  = HCLK / 2 = 36 MHz (SDClockPeriod = PERIOD_2)
 *   - Required interval = 64 ms / 4096 rows = 15.625 us
 *   - Counter = 15.625us * 36MHz - 20 = 542
 */
#include "bsp_sdram.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

/* Mode register field values (per SDRAM JEDEC standard). */
#define SDRAM_MODEREG_BURST_LENGTH_1          ((uint32_t)0x0000)
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL   ((uint32_t)0x0000)
#define SDRAM_MODEREG_CAS_LATENCY_3           ((uint32_t)0x0030)
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD ((uint32_t)0x0000)
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE  ((uint32_t)0x0200)

/* (15.625us * 36MHz) - 20 = 562 - 20 = 542 */
#define SDRAM_REFRESH_COUNT                   ((uint32_t)542)

int BSP_SDRAM_Init(void)
{
    FMC_SDRAM_CommandTypeDef cmd = {0};
    HAL_StatusTypeDef st;

    /* 1. Enable the SDRAM clock. */
    cmd.CommandMode          = FMC_SDRAM_CMD_CLK_ENABLE;
    cmd.CommandTarget        = FMC_SDRAM_CMD_TARGET_BANK2;
    cmd.AutoRefreshNumber    = 1;
    cmd.ModeRegisterDefinition = 0;
    st = HAL_SDRAM_SendCommand(&hsdram1, &cmd, 100);
    if (st != HAL_OK) { return 1; }

    /* 2. Minimum 100us stabilization delay (use 1ms for margin). */
    HAL_Delay(1);

    /* 3. Precharge all banks. */
    cmd.CommandMode = FMC_SDRAM_CMD_PALL;
    st = HAL_SDRAM_SendCommand(&hsdram1, &cmd, 100);
    if (st != HAL_OK) { return 2; }

    /* 4. Issue 8 consecutive auto-refresh commands. */
    cmd.CommandMode       = FMC_SDRAM_CMD_AUTOREFRESH_MODE;
    cmd.AutoRefreshNumber = 8;
    st = HAL_SDRAM_SendCommand(&hsdram1, &cmd, 100);
    if (st != HAL_OK) { return 3; }

    /* 5. Load the mode register. */
    cmd.CommandMode            = FMC_SDRAM_CMD_LOAD_MODE;
    cmd.AutoRefreshNumber      = 1;
    cmd.ModeRegisterDefinition = SDRAM_MODEREG_BURST_LENGTH_1
                              | SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL
                              | SDRAM_MODEREG_CAS_LATENCY_3
                              | SDRAM_MODEREG_OPERATING_MODE_STANDARD
                              | SDRAM_MODEREG_WRITEBURST_MODE_SINGLE;
    st = HAL_SDRAM_SendCommand(&hsdram1, &cmd, 100);
    if (st != HAL_OK) { return 4; }

    /* 6. Program the refresh timer so the FMC issues refreshes autonomously. */
    st = HAL_SDRAM_ProgramRefreshRate(&hsdram1, SDRAM_REFRESH_COUNT);
    if (st != HAL_OK) { return 5; }

    return 0;
}
