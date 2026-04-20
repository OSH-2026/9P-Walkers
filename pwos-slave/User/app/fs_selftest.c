#include "fs_selftest.h"

#include "lfs_port.hpp"
#include "sd_blockdev.hpp"
#include "lfs.h"

#include <stdbool.h>
#include <string.h>

#define FS_SELFTEST_DIR  "/verify"
#define FS_SELFTEST_FILE "/verify/fs_selftest.txt"

static const char g_fs_selftest_payload[] =
    "9P-Walkers LittleFS self-test over SDIO";

static uint32_t g_run_count = 0;

static int fs_selftest_write_file(lfs_t *lfs, FS_SelfTestReport *report) {
    lfs_file_t file;
    int err;
    lfs_ssize_t written;

    err = lfs_file_open(lfs, &file, FS_SELFTEST_FILE,
                        LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err < 0) {
        return err;
    }

    written = lfs_file_write(lfs, &file, g_fs_selftest_payload,
                             sizeof(g_fs_selftest_payload) - 1U);
    if (written < 0) {
        (void)lfs_file_close(lfs, &file);
        return (int)written;
    }

    report->bytes_written = (uint32_t)written;
    if ((uint32_t)written != (sizeof(g_fs_selftest_payload) - 1U)) {
        (void)lfs_file_close(lfs, &file);
        return LFS_ERR_IO;
    }

    err = lfs_file_sync(lfs, &file);
    if (err < 0) {
        (void)lfs_file_close(lfs, &file);
        return err;
    }

    return lfs_file_close(lfs, &file);
}

static int fs_selftest_stat_file(lfs_t *lfs, FS_SelfTestReport *report) {
    struct lfs_info info;
    int err;

    memset(&info, 0, sizeof(info));
    err = lfs_stat(lfs, FS_SELFTEST_FILE, &info);
    if (err < 0) {
        return err;
    }

    report->file_size = info.size;
    return 0;
}

static int fs_selftest_read_file(lfs_t *lfs, FS_SelfTestReport *report,
                                 char *buffer, uint32_t buffer_size) {
    lfs_file_t file;
    int err;
    lfs_ssize_t read_size;

    if (buffer_size < sizeof(g_fs_selftest_payload)) {
        return LFS_ERR_INVAL;
    }

    err = lfs_file_open(lfs, &file, FS_SELFTEST_FILE, LFS_O_RDONLY);
    if (err < 0) {
        return err;
    }

    memset(buffer, 0, buffer_size);
    read_size = lfs_file_read(lfs, &file, buffer,
                              sizeof(g_fs_selftest_payload) - 1U);
    if (read_size < 0) {
        (void)lfs_file_close(lfs, &file);
        return (int)read_size;
    }

    report->bytes_read = (uint32_t)read_size;
    if ((uint32_t)read_size != (sizeof(g_fs_selftest_payload) - 1U)) {
        (void)lfs_file_close(lfs, &file);
        return LFS_ERR_IO;
    }

    return lfs_file_close(lfs, &file);
}

static int fs_selftest_scan_dir(lfs_t *lfs, FS_SelfTestReport *report) {
    lfs_dir_t dir;
    struct lfs_info info;
    int err;
    bool found_file = false;

    err = lfs_dir_open(lfs, &dir, FS_SELFTEST_DIR);
    if (err < 0) {
        return err;
    }

    report->dir_entries = 0;
    while (1) {
        err = lfs_dir_read(lfs, &dir, &info);
        if (err < 0) {
            (void)lfs_dir_close(lfs, &dir);
            return err;
        }

        if (err == 0) {
            break;
        }

        report->dir_entries++;
        if (strcmp(info.name, "fs_selftest.txt") == 0) {
            found_file = true;
        }
    }

    err = lfs_dir_close(lfs, &dir);
    if (err < 0) {
        return err;
    }

    return found_file ? 0 : LFS_ERR_NOENT;
}

int fs_selftest_run(FS_SelfTestReport *report) {
    lfs_t *lfs;
    PW_SD_BlockDevInfoTypeDef blockdev_info;
    char readback_buffer[sizeof(g_fs_selftest_payload)];

    if (report == NULL) {
        return LFS_ERR_INVAL;
    }

    memset(report, 0, sizeof(*report));
    report->run_count = ++g_run_count;

    report->init_status = lfs_port_init();
    if (report->init_status != 0) {
        return report->init_status;
    }

    if (sd_blockdev_get_info(&blockdev_info) == 0) {
        report->sector_size = blockdev_info.sector_size;
        report->sector_count = blockdev_info.sector_count;
    }

    lfs = lfs_port_fs();

    report->mkdir_status = lfs_mkdir(lfs, FS_SELFTEST_DIR);
    if (report->mkdir_status == LFS_ERR_EXIST) {
        report->mkdir_status = 0;
    }

    report->write_status = fs_selftest_write_file(lfs, report);
    report->stat_status = fs_selftest_stat_file(lfs, report);
    report->read_status = fs_selftest_read_file(lfs, report, readback_buffer,
                                                sizeof(readback_buffer));
    report->dir_status = fs_selftest_scan_dir(lfs, report);

    if (report->read_status == 0 &&
        memcmp(readback_buffer, g_fs_selftest_payload,
               sizeof(g_fs_selftest_payload) - 1U) == 0) {
        report->compare_status = 0;
    } else {
        report->compare_status = LFS_ERR_CORRUPT;
    }

    if (report->stat_status == 0 &&
        report->file_size != (sizeof(g_fs_selftest_payload) - 1U)) {
        report->stat_status = LFS_ERR_CORRUPT;
    }

    if (report->init_status != 0) {
        return report->init_status;
    }
    if (report->mkdir_status != 0) {
        return report->mkdir_status;
    }
    if (report->write_status != 0) {
        return report->write_status;
    }
    if (report->stat_status != 0) {
        return report->stat_status;
    }
    if (report->read_status != 0) {
        return report->read_status;
    }
    if (report->dir_status != 0) {
        return report->dir_status;
    }

    return report->compare_status;
}
