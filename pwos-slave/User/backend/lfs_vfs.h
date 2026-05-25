/**
 * @file lfs_vfs.h
 * @brief 基于 littlefs 的从机本地 VFS backend。
 */

#ifndef LFS_VFS_H
#define LFS_VFS_H

#include <stdint.h>

#include "lfs.h"
#include "mini9p_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LFS_VFS_DEFAULT_IOUNIT 128u

struct lfs_vfs_config {
    uint16_t iounit;
};

struct lfs_vfs {
    lfs_t *lfs;
    uint16_t iounit;
};

void lfs_vfs_get_default_config(struct lfs_vfs_config *out_config);
int lfs_vfs_init(struct lfs_vfs *vfs, const struct lfs_vfs_config *config);
lfs_t *lfs_vfs_fs(struct lfs_vfs *vfs);
const struct m9p_server_ops *lfs_vfs_ops(void);

#ifdef __cplusplus
}
#endif

#endif