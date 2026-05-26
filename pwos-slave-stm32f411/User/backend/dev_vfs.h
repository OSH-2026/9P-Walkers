/**
 * @file dev_vfs.h
 * @brief Mini9P backend for node device controls.
 */

#ifndef DEV_VFS_H
#define DEV_VFS_H

#include <stdint.h>

#include "mini9p_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEV_VFS_DEFAULT_IOUNIT 128u

struct dev_vfs_device_ops {
    int (*read_led)(void *ctx, char *out, uint16_t out_cap, uint16_t *out_len);
    int (*write_led)(void *ctx, const uint8_t *data, uint16_t len, uint16_t *out_written);
};

struct dev_vfs_config {
    const struct dev_vfs_device_ops *device_ops;
    void *device_ctx;
    uint16_t iounit;
};

struct dev_vfs {
    const struct dev_vfs_device_ops *device_ops;
    void *device_ctx;
    uint16_t iounit;
};

int dev_vfs_init(struct dev_vfs *vfs, const struct dev_vfs_config *config);
const struct m9p_server_ops *dev_vfs_ops(void);

#ifdef __cplusplus
}
#endif

#endif
