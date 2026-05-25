/**
 * @file sys_vfs.h
 * @brief Mini9P backend for node system information.
 */

#ifndef SYS_VFS_H
#define SYS_VFS_H

#include <stdint.h>

#include "mini9p_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_VFS_DEFAULT_IOUNIT 128u

struct sys_vfs_config {
    const char *info_text;
    uint16_t iounit;
};

struct sys_vfs {
    const char *info_text;
    uint16_t iounit;
};

int sys_vfs_init(struct sys_vfs *vfs, const struct sys_vfs_config *config);
const struct m9p_server_ops *sys_vfs_ops(void);

#ifdef __cplusplus
}
#endif

#endif
