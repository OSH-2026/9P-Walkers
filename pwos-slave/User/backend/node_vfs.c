/**
 * @file node_vfs.c
 * @brief Mini9P mount router for /sys, /dev, /fs and legacy lfs paths.
 */

#include "node_vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lfs_port.hpp"

enum node_vfs_route_kind {
    NODE_VFS_ROUTE_ROOT = 0,
    NODE_VFS_ROUTE_SYS,
    NODE_VFS_ROUTE_DEV,
    NODE_VFS_ROUTE_LFS,
};

struct node_vfs_route {
    enum node_vfs_route_kind kind;
    const struct m9p_server_ops *ops;
    void *ctx;
    char path[M9P_MAX_PATH_LEN + 1u];
};

struct node_vfs_root_entry {
    const char *name;
    uint8_t qid_type;
    uint8_t flags;
    uint8_t perm;
    uint32_t object_id;
};

static const struct node_vfs_root_entry k_root_entries[] = {
    {"sys", M9P_QID_DIR | M9P_QID_VIRTUAL, M9P_STAT_DIR | M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 1u},
    {"dev", M9P_QID_DIR | M9P_QID_VIRTUAL, M9P_STAT_DIR | M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 2u},
    {"fs", M9P_QID_DIR, M9P_STAT_DIR, M9P_SERVER_PERM_READ, 3u},
};

static bool starts_with_mount_path(const char *path, const char *mount)
{
    size_t mount_len;

    if (path == NULL || mount == NULL) {
        return false;
    }

    mount_len = strlen(mount);
    return strncmp(path, mount, mount_len) == 0 &&
           (path[mount_len] == '\0' || path[mount_len] == '/');
}

static int copy_path(const char *src, char *dst, size_t dst_cap)
{
    if (src == NULL || dst == NULL || dst_cap == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (strlen(src) >= dst_cap) {
        return -(int)M9P_ERR_EMSIZE;
    }

    strcpy(dst, src);
    return 0;
}

static int resolve_route(struct node_vfs *vfs, const char *path, struct node_vfs_route *out_route)
{
    const char *mapped_path = path;

    if (vfs == NULL || path == NULL || out_route == NULL || path[0] != '/') {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(out_route, 0, sizeof(*out_route));
    if (strcmp(path, "/") == 0) {
        out_route->kind = NODE_VFS_ROUTE_ROOT;
        return copy_path(path, out_route->path, sizeof(out_route->path));
    }

    if (starts_with_mount_path(path, "/sys")) {
        if (vfs->sys_ops == NULL) {
            return -(int)M9P_ERR_ENOTSUP;
        }
        out_route->kind = NODE_VFS_ROUTE_SYS;
        out_route->ops = vfs->sys_ops;
        out_route->ctx = vfs->sys_ctx;
        return copy_path(path, out_route->path, sizeof(out_route->path));
    }

    if (starts_with_mount_path(path, "/dev")) {
        if (vfs->dev_ops == NULL) {
            return -(int)M9P_ERR_ENOTSUP;
        }
        out_route->kind = NODE_VFS_ROUTE_DEV;
        out_route->ops = vfs->dev_ops;
        out_route->ctx = vfs->dev_ctx;
        return copy_path(path, out_route->path, sizeof(out_route->path));
    }

    if (starts_with_mount_path(path, "/fs")) {
        mapped_path = path + 3u;
        if (mapped_path[0] == '\0') {
            mapped_path = "/";
        }
    }

    if (vfs->lfs_ops == NULL) {
        return -(int)M9P_ERR_ENOTSUP;
    }
    out_route->kind = NODE_VFS_ROUTE_LFS;
    out_route->ops = vfs->lfs_ops;
    out_route->ctx = vfs->lfs_ctx;
    return copy_path(mapped_path, out_route->path, sizeof(out_route->path));
}

static void fill_root_stat(struct m9p_stat *out_stat)
{
    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->qid.type = M9P_QID_DIR | M9P_QID_VIRTUAL;
    out_stat->qid.version = 1u;
    out_stat->qid.object_id = 0u;
    out_stat->perm = M9P_SERVER_PERM_READ;
    out_stat->flags = M9P_STAT_DIR | M9P_STAT_VIRTUAL;
    out_stat->size = 0u;
    (void)snprintf(out_stat->name, sizeof(out_stat->name), "/");
}

static int append_root_dirent(const struct node_vfs_root_entry *entry,
                              uint32_t *stream_offset,
                              uint32_t read_offset,
                              uint8_t *out_data,
                              uint16_t out_cap,
                              uint16_t *out_count)
{
    uint8_t record[11u + M9P_MAX_NAME_LEN];
    struct m9p_dirent dirent;
    size_t record_len;
    size_t start;
    size_t available;

    memset(&dirent, 0, sizeof(dirent));
    dirent.qid.type = entry->qid_type;
    dirent.qid.version = 1u;
    dirent.qid.object_id = entry->object_id;
    dirent.perm = entry->perm;
    dirent.flags = entry->flags;
    (void)snprintf(dirent.name, sizeof(dirent.name), "%s", entry->name);
    if (!m9p_build_dirent(&dirent, record, sizeof(record), &record_len)) {
        return -(int)M9P_ERR_EIO;
    }

    if (*stream_offset + record_len <= read_offset) {
        *stream_offset += (uint32_t)record_len;
        return 0;
    }

    start = read_offset > *stream_offset ? (size_t)(read_offset - *stream_offset) : 0u;
    available = record_len - start;
    if (available > (size_t)(out_cap - *out_count)) {
        available = (size_t)(out_cap - *out_count);
    }

    if (available > 0u && out_data != NULL) {
        memcpy(out_data + *out_count, record + start, available);
        *out_count = (uint16_t)(*out_count + available);
    }

    *stream_offset += (uint32_t)record_len;
    return *out_count == out_cap ? 1 : 0;
}

static int read_root_dir(struct node_vfs *vfs,
                         uint32_t offset,
                         uint8_t *out_data,
                         uint16_t out_cap,
                         uint16_t *out_count)
{
    uint32_t stream_offset = 0u;
    size_t i;

    if (out_count == NULL || (out_cap > 0u && out_data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    for (i = 0u; i < sizeof(k_root_entries) / sizeof(k_root_entries[0]); ++i) {
        /* Skip /fs entry when LFS backend is not available. */
        if (strcmp(k_root_entries[i].name, "fs") == 0 && vfs->lfs_ops == NULL) {
            continue;
        }
        {
            int rc = append_root_dirent(&k_root_entries[i], &stream_offset, offset, out_data, out_cap, out_count);
            if (rc != 0) {
                return rc > 0 ? 0 : rc;
            }
        }
    }
    return 0;
}

static int node_stat_cb(void *ctx, const char *path, struct m9p_stat *out_stat)
{
    struct node_vfs *vfs = (struct node_vfs *)ctx;
    struct node_vfs_route route;
    int rc;

    if (out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = resolve_route(vfs, path, &route);
    if (rc != 0) {
        return rc;
    }
    if (route.kind == NODE_VFS_ROUTE_ROOT) {
        fill_root_stat(out_stat);
        return 0;
    }
    return route.ops->stat(route.ctx, route.path, out_stat);
}

static int node_open_cb(void *ctx,
                        const char *path,
                        uint8_t mode,
                        struct m9p_qid *out_qid,
                        uint16_t *out_iounit)
{
    struct node_vfs *vfs = (struct node_vfs *)ctx;
    struct node_vfs_route route;
    struct m9p_stat stat;
    int rc;

    if (out_qid == NULL || out_iounit == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = resolve_route(vfs, path, &route);
    if (rc != 0) {
        return rc;
    }
    if (route.kind != NODE_VFS_ROUTE_ROOT) {
        return route.ops->open(route.ctx, route.path, mode, out_qid, out_iounit);
    }

    if ((mode & 0x03u) != M9P_OREAD || (mode & M9P_OTRUNC) != 0u) {
        return -(int)M9P_ERR_EISDIR;
    }
    fill_root_stat(&stat);
    *out_qid = stat.qid;
    *out_iounit = vfs->iounit;
    return 0;
}

static int node_read_cb(void *ctx,
                        const char *path,
                        uint32_t offset,
                        uint8_t mode,
                        uint8_t *out_data,
                        uint16_t out_cap,
                        uint16_t *out_count)
{
    struct node_vfs *vfs = (struct node_vfs *)ctx;
    struct node_vfs_route route;
    int rc;

    rc = resolve_route(vfs, path, &route);
    if (rc != 0) {
        return rc;
    }
    if (route.kind == NODE_VFS_ROUTE_ROOT) {
        return read_root_dir(vfs, offset, out_data, out_cap, out_count);
    }
    return route.ops->read(route.ctx, route.path, offset, mode, out_data, out_cap, out_count);
}

static int node_write_cb(void *ctx,
                         const char *path,
                         uint32_t offset,
                         uint8_t mode,
                         const uint8_t *data,
                         uint16_t count,
                         uint16_t *out_count)
{
    struct node_vfs *vfs = (struct node_vfs *)ctx;
    struct node_vfs_route route;
    int rc;

    rc = resolve_route(vfs, path, &route);
    if (rc != 0) {
        return rc;
    }
    if (route.kind == NODE_VFS_ROUTE_ROOT) {
        if (out_count != NULL) {
            *out_count = 0u;
        }
        return -(int)M9P_ERR_EISDIR;
    }
    return route.ops->write(route.ctx, route.path, offset, mode, data, count, out_count);
}

static int node_clunk_cb(void *ctx, const char *path, bool was_open)
{
    struct node_vfs *vfs = (struct node_vfs *)ctx;
    struct node_vfs_route route;
    int rc;

    rc = resolve_route(vfs, path, &route);
    if (rc != 0) {
        return rc;
    }
    if (route.kind == NODE_VFS_ROUTE_ROOT) {
        return 0;
    }
    return route.ops->clunk(route.ctx, route.path, was_open);
}

static const struct m9p_server_ops k_node_vfs_ops = {
    node_stat_cb,
    node_open_cb,
    node_read_cb,
    node_write_cb,
    node_clunk_cb,
};

int node_vfs_init(struct node_vfs *vfs, const struct node_vfs_config *config)
{
    struct sys_vfs_config sys_config;
    struct dev_vfs_config dev_config;
    struct lfs_vfs *active_lfs = NULL;
    int rc;

    if (vfs == NULL || config == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(vfs, 0, sizeof(*vfs));

    struct lfs_vfs_config lfs_config;

    lfs_vfs_get_default_config(&lfs_config);
    rc = lfs_vfs_init(&vfs->lfs_vfs, &lfs_config);
    if (rc == 0) {
#ifdef PWOS_ENABLE_LFS_SELFTEST
        (void)fs_selftest_run_on_fs(lfs_vfs_fs(&vfs->lfs_vfs), lfs_port_backend_name(), &vfs->fs_report);
#endif
        active_lfs = &vfs->lfs_vfs;
    }

    memset(&sys_config, 0, sizeof(sys_config));
    sys_config.info_text = active_lfs != NULL ? "pwos node lfs=ok\n" : "pwos node lfs=unavailable\n";
    sys_config.routes_text_fn = config->routes_text_fn;
    sys_config.routes_text_ctx = config->routes_text_ctx;
    sys_config.log_text_fn = config->log_text_fn;
    sys_config.log_text_ctx = config->log_text_ctx;
    sys_config.uart_text_fn = config->uart_text_fn;
    sys_config.uart_text_ctx = config->uart_text_ctx;
    sys_config.iounit = SYS_VFS_DEFAULT_IOUNIT;
    rc = sys_vfs_init(&vfs->sys_vfs, &sys_config);
    if (rc != 0) {
        return rc;
    }

    memset(&dev_config, 0, sizeof(dev_config));
    dev_config.iounit = DEV_VFS_DEFAULT_IOUNIT;
    rc = dev_vfs_init(&vfs->dev_vfs, &dev_config);
    if (rc != 0) {
        return rc;
    }

    vfs->sys_ops = sys_vfs_ops();
    vfs->sys_ctx = &vfs->sys_vfs;
    vfs->dev_ops = dev_vfs_ops();
    vfs->dev_ctx = &vfs->dev_vfs;
    if (active_lfs != NULL) {
        vfs->lfs_ops = lfs_vfs_ops();
        vfs->lfs_ctx = active_lfs;
    }
    vfs->iounit = config->iounit == 0u ? NODE_VFS_DEFAULT_IOUNIT : config->iounit;
    return 0;
}

const struct m9p_server_ops *node_vfs_ops(void)
{
    return &k_node_vfs_ops;
}
