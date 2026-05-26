/**
 * @file node_vfs.h
 * @brief Mini9P mount router for node-local backends.
 */

#ifndef NODE_VFS_H
#define NODE_VFS_H

#include <stdint.h>

#include "mini9p_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_VFS_DEFAULT_IOUNIT 128u

struct node_vfs_config {
    const struct m9p_server_ops *sys_ops;
    void *sys_ctx;
    const struct m9p_server_ops *dev_ops;
    void *dev_ctx;
    const struct m9p_server_ops *lfs_ops;
    void *lfs_ctx;
    uint16_t iounit;
};

struct node_vfs {
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
