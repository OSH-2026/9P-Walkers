/**
 * @file node_vfs.h
 * @brief Mini9P mount router for node-local backends.
 */

#ifndef NODE_VFS_H
#define NODE_VFS_H

#include <stdint.h>

#include "dev_vfs.h"
#include "fs_selftest.h"
#include "lfs_vfs.h"
#include "mini9p_server.h"
#include "sys_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_VFS_DEFAULT_IOUNIT 128u

struct node_vfs_config {
    int (*routes_text_fn)(void *ctx, char *out, size_t out_cap);
    void *routes_text_ctx;
    int (*log_text_fn)(void *ctx, char *out, size_t out_cap);
    void *log_text_ctx;
    int (*uart_text_fn)(void *ctx, char *out, size_t out_cap);
    void *uart_text_ctx;
    uint16_t iounit;
};

struct node_vfs {
    struct sys_vfs sys_vfs;
    struct dev_vfs dev_vfs;
    struct lfs_vfs lfs_vfs;
    FS_SelfTestReport fs_report;
    const struct m9p_server_ops *sys_ops;
    void *sys_ctx;
    const struct m9p_server_ops *dev_ops;
    void *dev_ctx;
    const struct m9p_server_ops *lfs_ops;
    void *lfs_ctx;
    uint16_t iounit;
};

int node_vfs_init(struct node_vfs *vfs, const struct node_vfs_config *config);
const struct m9p_server_ops *node_vfs_ops(void);

#ifdef __cplusplus
}
#endif

#endif
