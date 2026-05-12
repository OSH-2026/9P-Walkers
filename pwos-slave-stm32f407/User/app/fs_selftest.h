/**
 * @file fs_selftest.h
 * @author hb (huobin92@gmail.com)
 * @brief GPT-Generated test bench for lfs
 * @version 0.1
 * @date 2026-04-21
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#ifndef FS_SELFTEST_H
#define FS_SELFTEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t run_count;
    int32_t init_status;
    int32_t mkdir_status;
    int32_t write_status;
    int32_t stat_status;
    int32_t read_status;
    int32_t dir_status;
    int32_t compare_status;
    uint32_t file_size;
    uint32_t bytes_written;
    uint32_t bytes_read;
} FS_SelfTestReport;

int fs_selftest_run(FS_SelfTestReport *report);

#ifdef __cplusplus
}
#endif

#endif
