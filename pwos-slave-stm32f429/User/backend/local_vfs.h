/**
 * @file local_vfs.h
 * @brief 极简从机虚拟 VFS，通过 Mini9P 对外暴露。
 *
 * local_vfs v1 仅暴露本地虚拟节点。它不挂载 littlefs、不持有持久文件、
 * 也不保留后端文件句柄。Mini9P 服务器负责 fid/会话状态，并通过
 * m9p_server_ops 调用本模块。
 */

#ifndef LOCAL_VFS_H
#define LOCAL_VFS_H

#include <stdint.h>

#include "mini9p_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 当 config.iounit 为零时，Ropen 中通告的默认 I/O 单元大小。 */
#define LOCAL_VFS_DEFAULT_IOUNIT 256u

typedef int (*local_vfs_read_fn)(
    void *ctx,
    const char *path,
    uint32_t offset,
    uint8_t *out_data,
    uint16_t out_cap,
    uint16_t *out_count);

typedef int (*local_vfs_write_fn)(
    void *ctx,
    const char *path,
    uint32_t offset,
    const uint8_t *data,
    uint16_t count,
    uint16_t *out_count);

/** @brief local_vfs 初始化参数。 */
struct local_vfs_config {
    uint16_t iounit; /**< 每次读取的期望载荷大小；为零则使用 LOCAL_VFS_DEFAULT_IOUNIT。 */
    void *io_ctx;
    local_vfs_read_fn read;
    local_vfs_write_fn write;
};

/**
 * @brief 极简本地 VFS 的运行时状态。
 */
struct local_vfs {
    uint16_t iounit; /**< 通告的 I/O 单元大小。 */
    void *io_ctx;
    local_vfs_read_fn read;
    local_vfs_write_fn write;
};

/**
 * @brief 用默认值填充 local_vfs_config。
 *
 * @param[out] out_config 目标配置。NULL 将被忽略。
 */
void local_vfs_get_default_config(struct local_vfs_config *out_config);

/**
 * @brief 初始化 local_vfs 实例。
 *
 * @param[out] vfs    要初始化的 VFS 实例。
 * @param[in] config 可选配置。NULL 表示使用默认值。
 * @return 成功返回 0，失败返回负的 Mini9P 错误码。
 */
int local_vfs_init(struct local_vfs *vfs, const struct local_vfs_config *config);

/**
 * @brief 返回 local_vfs 实现的 Mini9P 服务器回调表。
 *
 * 将该指针传给 m9p_server_config::ops，并将 local_vfs 实例传给
 * m9p_server_config::ops_ctx。
 *
 * @return 指向静态 m9p_server_ops 表的指针。
 */
const struct m9p_server_ops *local_vfs_ops(void);

#ifdef __cplusplus
}
#endif

#endif
