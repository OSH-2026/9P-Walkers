/**
 * @file local_vfs.h
 * @brief Minimal slave-side virtual VFS exposed through Mini9P.
 *
 * local_vfs v1 exposes only local virtual nodes. It does not mount littlefs,
 * own persistent files, or keep backend file handles. The Mini9P server owns
 * fid/session state and calls this module through m9p_server_ops.
 */

#ifndef LOCAL_VFS_H
#define LOCAL_VFS_H

#include <stdint.h>

#include "mini9p_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default I/O unit advertised through Ropen when config.iounit is zero. */
#define LOCAL_VFS_DEFAULT_IOUNIT 128u

/**
 * @brief local_vfs initialization parameters.
 */
struct local_vfs_config {
    uint16_t iounit; /**< Preferred per-read payload size; zero selects LOCAL_VFS_DEFAULT_IOUNIT. */
};

/**
 * @brief Runtime state for the minimal local VFS.
 */
struct local_vfs {
    uint16_t iounit; /**< Advertised I/O unit. */
};

/**
 * @brief Fill a local_vfs_config with defaults.
 *
 * @param[out] out_config Destination config. NULL is ignored.
 */
void local_vfs_get_default_config(struct local_vfs_config *out_config);

/**
 * @brief Initialize a local_vfs instance.
 *
 * @param[out] vfs    VFS instance to initialize.
 * @param[in] config Optional config. NULL uses defaults.
 * @return 0 on success, negative Mini9P error code on failure.
 */
int local_vfs_init(struct local_vfs *vfs, const struct local_vfs_config *config);

/**
 * @brief Return the Mini9P server callback table implemented by local_vfs.
 *
 * Pass this pointer to m9p_server_config::ops and the local_vfs instance to
 * m9p_server_config::ops_ctx.
 *
 * @return Pointer to a static m9p_server_ops table.
 */
const struct m9p_server_ops *local_vfs_ops(void);

#ifdef __cplusplus
}
#endif

#endif
