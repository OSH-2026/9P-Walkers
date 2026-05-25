/**
 * @file lfs_port.cpp
 * @author hb (huobin92@gmail.com)
 * @brief 
 * @version 0.1
 * @date 2026-04-20
 * 
 * @copyright Copyright (c) 2026
 * 
 */
#include "lfs_port.hpp"

#if defined(PWOS_LFS_PORT_USE_SD)
#include "sd_blockdev.hpp"
#endif

#include <string.h>

#if defined(PWOS_LFS_PORT_USE_SD)

#define LFS_PORT_BACKEND_NAME "sd"
#define SD_SECTOR_SIZE      512U
#define LFS_READ_SIZE       512U
#define LFS_PROG_SIZE       512U
#define LFS_BLOCK_SIZE      4096U
#define LFS_BLOCK_COUNT     0U
#define LFS_CACHE_SIZE      512U
#define LFS_LOOKAHEAD_SIZE  32U

#else

#define LFS_PORT_BACKEND_NAME "ram"
#define LFS_READ_SIZE       16U
#define LFS_PROG_SIZE       16U
#define LFS_BLOCK_SIZE      256U
#define LFS_BLOCK_COUNT     64U
#define LFS_CACHE_SIZE      64U
#define LFS_LOOKAHEAD_SIZE  16U

#endif

// Global LittleFS 实例
static lfs_t g_lfs;
static struct lfs_config g_cfg;
static bool g_lfs_initialized;

// 全局缓存
static uint8_t g_read_buffer[LFS_CACHE_SIZE];
static uint8_t g_prog_buffer[LFS_CACHE_SIZE];
static uint8_t g_lookahead_buffer[LFS_LOOKAHEAD_SIZE];

#if defined(PWOS_LFS_PORT_USE_SD)

// 全局 SD 卡相关静态缓冲区
static uint8_t g_erase_buffer[SD_SECTOR_SIZE];

/**
 * @brief SD 卡块设备读取函数，输入 LFS 块号和块内偏移量，表示从特定块号的特定字节位置开始读写，位置必须按 Secotr 对齐，然后换算成 Sector
 * 
 * @param cfg 
 * @param block 块号(LFS 块号)
 * @param off 偏移量，用于块内寻址 (按 Byte)
 * @param buffer 缓冲区指针
 * @param size 数据大小
 * @return int 返回状态
 */
static int lfs_sd_read(const struct lfs_config *cfg, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    uint32_t byte_addr;
    uint32_t sector;
    uint32_t count;

    // off 和 size 按 Sector 对齐
    if ((off % SD_SECTOR_SIZE) != 0 || (size % SD_SECTOR_SIZE) != 0) {
        return LFS_ERR_IO;
    }

    byte_addr = block * cfg->block_size + off;
    sector = byte_addr / SD_SECTOR_SIZE;
    count = size / SD_SECTOR_SIZE;

    if (sd_blockdev_read(sector, buffer, count) != 0) {
        return LFS_ERR_IO;
    }

    return 0;
}

/**
 * @brief SD 卡块设备写函数
 * 
 * @param cfg 
 * @param block 
 * @param off 
 * @param buffer 
 * @param size 
 * @return int 
 */
static int lfs_sd_prog(const struct lfs_config *cfg,
                       lfs_block_t block,
                       lfs_off_t off,
                       const void *buffer,
                       lfs_size_t size)
{
    uint32_t byte_addr;
    uint32_t sector;
    uint32_t count;

    if ((off % SD_SECTOR_SIZE) != 0 || (size % SD_SECTOR_SIZE) != 0) {
        return LFS_ERR_IO;
    }

    byte_addr = block * cfg->block_size + off;
    sector = byte_addr / SD_SECTOR_SIZE;
    count = size / SD_SECTOR_SIZE;

    if (sd_blockdev_write(sector, buffer, count) != 0) {
        return LFS_ERR_IO;
    }

    return 0;
}

/**
 * @brief SD 卡块设备擦除函数，按 LFS 块号擦除
 * 
 * @param cfg 
 * @param block LFS 块号
 * @return int 
 */
static int lfs_sd_erase(const struct lfs_config *cfg, lfs_block_t block)
{
    // 得到第一个扇区号和每个扇区内的块数量
    uint32_t first_sector = (block * cfg->block_size) / SD_SECTOR_SIZE;
    uint32_t sectors_per_block = cfg->block_size / SD_SECTOR_SIZE;
    uint32_t i;

    memset(g_erase_buffer, 0xff, sizeof(g_erase_buffer));

    // 按 Sector 擦除
    for (i = 0; i < sectors_per_block; ++i) {
        if (sd_blockdev_write(first_sector + i, g_erase_buffer, 1) != 0) {
            return LFS_ERR_IO;
        }
    }

    return 0;
}

/**
 * @brief SD 卡块设备读同步函数
 * 
 * @param cfg 
 * @return int 
 */
static int lfs_sd_sync(const struct lfs_config *cfg)
{
    (void)cfg;
    return sd_blockdev_sync() == 0 ? 0 : LFS_ERR_IO;
}

/**
 * @brief SD 卡块设备初始化函数
 * 
 * @return int 
 */
int lfs_port_init(void)
{
    PW_SD_BlockDevInfoTypeDef info;
    int err_code;

    if (g_lfs_initialized) {
        return 0;
    }

    if (sd_blockdev_init() != 0) {
        return -10;
    }

    if (sd_blockdev_get_info(&info) != 0) {
        return -11;
    }

    if (info.sector_size != SD_SECTOR_SIZE) {
        return -12;
    }

    if (info.sector_count < (LFS_BLOCK_SIZE / SD_SECTOR_SIZE)) {
        return -13;
    }

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.read = lfs_sd_read;
    g_cfg.prog = lfs_sd_prog;
    g_cfg.erase = lfs_sd_erase;
    g_cfg.sync = lfs_sd_sync;

    g_cfg.read_size = LFS_READ_SIZE;
    g_cfg.prog_size = LFS_PROG_SIZE;
    g_cfg.block_size = LFS_BLOCK_SIZE;
    g_cfg.block_count = info.sector_count / (LFS_BLOCK_SIZE / SD_SECTOR_SIZE);
    g_cfg.cache_size = LFS_CACHE_SIZE;
    g_cfg.lookahead_size = LFS_LOOKAHEAD_SIZE;
    g_cfg.block_cycles = -1;

    g_cfg.read_buffer = g_read_buffer;
    g_cfg.prog_buffer = g_prog_buffer;
    g_cfg.lookahead_buffer = g_lookahead_buffer;

    err_code = lfs_mount(&g_lfs, &g_cfg);
    if (err_code != 0) {
        err_code = lfs_format(&g_lfs, &g_cfg);
        if (err_code != 0) {
            return err_code;
        }

        err_code = lfs_mount(&g_lfs, &g_cfg);
        if (err_code != 0) {
            return err_code;
        }
    }

    g_lfs_initialized = true;
    return 0;
}

/**
 * @brief LFS 对象指针获取函数
 * 
 * @return lfs_t* 
 */
lfs_t *lfs_port_fs(void)
{
    return &g_lfs;
}

#else

static uint8_t g_ram_storage[LFS_BLOCK_SIZE * LFS_BLOCK_COUNT];
static bool g_ram_storage_initialized;

static int lfs_ram_read(const struct lfs_config *cfg,
                        lfs_block_t block,
                        lfs_off_t off,
                        void *buffer,
                        lfs_size_t size)
{
    size_t base;

    if (cfg == NULL || buffer == NULL || block >= LFS_BLOCK_COUNT || off + size > cfg->block_size) {
        return LFS_ERR_IO;
    }

    base = (size_t)block * cfg->block_size + off;
    memcpy(buffer, g_ram_storage + base, size);
    return 0;
}

static int lfs_ram_prog(const struct lfs_config *cfg,
                        lfs_block_t block,
                        lfs_off_t off,
                        const void *buffer,
                        lfs_size_t size)
{
    size_t base;

    if (cfg == NULL || buffer == NULL || block >= LFS_BLOCK_COUNT || off + size > cfg->block_size) {
        return LFS_ERR_IO;
    }

    base = (size_t)block * cfg->block_size + off;
    memcpy(g_ram_storage + base, buffer, size);
    return 0;
}

static int lfs_ram_erase(const struct lfs_config *cfg, lfs_block_t block)
{
    size_t base;

    if (cfg == NULL || block >= LFS_BLOCK_COUNT) {
        return LFS_ERR_IO;
    }

    base = (size_t)block * cfg->block_size;
    memset(g_ram_storage + base, 0xff, cfg->block_size);
    return 0;
}

static int lfs_ram_sync(const struct lfs_config *cfg)
{
    (void)cfg;
    return 0;
}

int lfs_port_init(void)
{
    int err_code;

    if (g_lfs_initialized) {
        return 0;
    }

    if (!g_ram_storage_initialized) {
        memset(g_ram_storage, 0xff, sizeof(g_ram_storage));
        g_ram_storage_initialized = true;
    }

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.read = lfs_ram_read;
    g_cfg.prog = lfs_ram_prog;
    g_cfg.erase = lfs_ram_erase;
    g_cfg.sync = lfs_ram_sync;

    g_cfg.read_size = LFS_READ_SIZE;
    g_cfg.prog_size = LFS_PROG_SIZE;
    g_cfg.block_size = LFS_BLOCK_SIZE;
    g_cfg.block_count = LFS_BLOCK_COUNT;
    g_cfg.cache_size = LFS_CACHE_SIZE;
    g_cfg.lookahead_size = LFS_LOOKAHEAD_SIZE;
    g_cfg.block_cycles = -1;

    g_cfg.read_buffer = g_read_buffer;
    g_cfg.prog_buffer = g_prog_buffer;
    g_cfg.lookahead_buffer = g_lookahead_buffer;

    err_code = lfs_mount(&g_lfs, &g_cfg);
    if (err_code != 0) {
        err_code = lfs_format(&g_lfs, &g_cfg);
        if (err_code != 0) {
            return err_code;
        }

        err_code = lfs_mount(&g_lfs, &g_cfg);
        if (err_code != 0) {
            return err_code;
        }
    }

    g_lfs_initialized = true;
    return 0;
}

lfs_t *lfs_port_fs(void)
{
    return &g_lfs;
}

#endif

const char *lfs_port_backend_name(void)
{
    return LFS_PORT_BACKEND_NAME;
}
