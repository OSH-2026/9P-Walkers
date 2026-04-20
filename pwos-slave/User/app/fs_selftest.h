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
    uint32_t dir_entries;
    uint32_t sector_size;
    uint32_t sector_count;
} FS_SelfTestReport;

int fs_selftest_run(FS_SelfTestReport *report);

#ifdef __cplusplus
}
#endif

#endif
