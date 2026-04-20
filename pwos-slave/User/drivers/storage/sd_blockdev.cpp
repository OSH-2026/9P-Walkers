/**
 * @file sd_blockdev.c
 * @author hb (huobin92@gmail.com)
 * @brief 
 * @version 0.1
 * @date 2026-04-20
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#include "sd_blockdev.hpp"
#include "sdio.h"
#include "stm32f4xx_hal_sd.h"

extern SD_HandleTypeDef hsd;

#define SD_BLOCKDEV_TIMEOUT_MS 1000
/**
 * @brief SD 卡准备等待函数 (Ms)
 * 
 * @param timeout_ms 
 * @return int 
 */
static int sd_wait_ready(uint32_t timeout_ms) {
    uint32_t start = HAL_GetTick();

    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {
        if ((HAL_GetTick() - start) > timeout_ms) {
            return -1;
        }
    }

    return 0;
}

/**
 * @brief SD 卡块设备初始化函数
 * 
 * @return int 
 */
int sd_blockdev_init(void) {
    HAL_SD_CardInfoTypeDef card_info;

    if (HAL_SD_GetCardInfo(&hsd, &card_info) != HAL_OK) {
        return -1;
    }

    return sd_wait_ready(SD_BLOCKDEV_TIMEOUT_MS);
}

/**
 * @brief SD 卡块设备信息获取函数
 * 
 * @param info 
 * @return int 
 */
int sd_blockdev_get_info(PW_SD_BlockDevInfoTypeDef* info) {
    HAL_SD_CardInfoTypeDef card_info;

    if (info == 0) {
        return -1;
    }

    if (HAL_SD_GetCardInfo(&hsd, &card_info) != HAL_OK) {
        return -1;
    }

    info->sector_count = card_info.LogBlockNbr;
    info->sector_size = card_info.LogBlockSize;

    return 0;
}

/**
 * @brief SD 卡块设备读取函数
 * 
 * @param sector 起始扇区
 * @param buffer 数据缓冲区
 * @param sector_count 扇区数量
 * @return int 0 表示成功，-1 表示失败
 */
int sd_blockdev_read(uint32_t sector, void *buffer, uint32_t sector_count) {
    if (HAL_SD_ReadBlocks(&hsd, (uint8_t *)buffer, sector, sector_count,
                          SD_BLOCKDEV_TIMEOUT_MS) != HAL_OK) {
        return -1;
    }

    return sd_wait_ready(SD_BLOCKDEV_TIMEOUT_MS);
}

/**
 * @brief SD 卡块设备写入函数
 * 
 * @param sector 起始扇区
 * @param buffer 数据缓冲区
 * @param sector_count 扇区数量
 * @return int 0 表示成功，-1 表示失败
 */
int sd_blockdev_write(uint32_t sector, const void *buffer, uint32_t sector_count) {
    if (HAL_SD_WriteBlocks(&hsd, (uint8_t *)buffer, sector, sector_count,
                           SD_BLOCKDEV_TIMEOUT_MS) != HAL_OK) {
        return -1;
    }

    return sd_wait_ready(SD_BLOCKDEV_TIMEOUT_MS);
}

int sd_blockdev_sync(void) {
    
    return sd_wait_ready(SD_BLOCKDEV_TIMEOUT_MS);
}

// TODO: 修改阻塞为非阻塞读取
