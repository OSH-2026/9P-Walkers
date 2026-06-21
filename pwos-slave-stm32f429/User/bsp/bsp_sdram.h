/**
 * @file bsp_sdram.h
 * @brief SDRAM (IS42S16400J) initialization for STM32F429I-DISCO.
 *
 * CubeMX only configures the FMC peripheral; the SDRAM chip itself still
 * needs an initialization command sequence (clock enable -> precharge all ->
 * auto refresh -> load mode register) plus a programmed refresh rate before
 * the memory at 0xD0000000 can be used. This module performs that sequence.
 */
#ifndef BSP_SDRAM_H_
#define BSP_SDRAM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "fmc.h"
#include <stdint.h>

/* Framebuffer lives in FMC SDRAM Bank 2 (configured by CubeMX). */
#define BSP_SDRAM_BASE_ADDR      0xD0000000UL
#define BSP_SDRAM_SIZE_BYTES     (8UL * 1024UL * 1024UL)  /* IS42S16400J: 64Mbit = 8MB, 16-bit bus */

/**
 * @brief Run the SDRAM chip init sequence and program the refresh rate.
 * @return 0 on success, non-zero on HAL error.
 * @note Must be called AFTER MX_FMC_Init() and before any framebuffer access.
 */
int BSP_SDRAM_Init(void);

#ifdef __cplusplus
}
#endif
#endif /* BSP_SDRAM_H_ */
