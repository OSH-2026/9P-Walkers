/**
 * @file sd_blockdev.h
 * @author hb (huobin92@gmail.com)
 * @brief 
 * @version 0.1
 * @date 2026-04-20
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#ifndef SD_BLOCKDEV_HPP
#define SD_BLOCKDEV_HPP

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sector_size;
    uint32_t sector_count;
} PW_SD_BlockDevInfoTypeDef;

int sd_blockdev_init(void);
int sd_blockdev_get_info(PW_SD_BlockDevInfoTypeDef* info);
int sd_blockdev_read(uint32_t sector, void *buffer, uint32_t sector_count);
int sd_blockdev_write(uint32_t sector, const void *buffer, uint32_t sector_count);
int sd_blockdev_sync(void);

#ifdef __cplusplus
}
#endif

#endif
