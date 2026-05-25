/**
 * @file lfs_vfs.c
 * @brief 基于 littlefs 的 Mini9P backend。
 */

#include "lfs_vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lfs_port.hpp"

static uint32_t fnv1a32(const char *text)
{
    uint32_t hash = 2166136261u;

    if (text == NULL) {
        return 1u;
    }

    while (*text != '\0') {
        hash ^= (uint8_t)*text;
        hash *= 16777619u;
        ++text;
    }

    return hash == 0u ? 1u : hash;
}

static bool is_root_path(const char *path)
{
    return path != NULL && strcmp(path, "/") == 0;
}

static const char *node_name_from_path(const char *path)
{
    const char *name = path;
    const char *p = path;

    if (path == NULL || strcmp(path, "/") == 0) {
        return "/";
    }

    while (*p != '\0') {
        if (*p == '/' && p[1] != '\0') {
            name = p + 1;
        }
        ++p;
    }

    return name;
}

static int lfs_error_to_m9p(int rc)
{
    if (rc >= 0) {
        return 0;
    }

    switch (rc) {
    case LFS_ERR_NOENT:
        return -(int)M9P_ERR_ENOENT;
    case LFS_ERR_EXIST:
    case LFS_ERR_NOTEMPTY:
    case LFS_ERR_NOSPC:
    case LFS_ERR_NOMEM:
        return -(int)M9P_ERR_EBUSY;
    case LFS_ERR_NOTDIR:
        return -(int)M9P_ERR_ENOTDIR;
    case LFS_ERR_ISDIR:
        return -(int)M9P_ERR_EISDIR;
    case LFS_ERR_BADF:
        return -(int)M9P_ERR_EFID;
    case LFS_ERR_INVAL:
    case LFS_ERR_NAMETOOLONG:
        return -(int)M9P_ERR_EINVAL;
    case LFS_ERR_FBIG:
        return -(int)M9P_ERR_EMSIZE;
    default:
        return -(int)M9P_ERR_EIO;
    }
}

static int join_child_path(const char *dir_path,
                           const char *name,
                           char *out_path,
                           size_t out_cap)
{
    int written;

    if (dir_path == NULL || name == NULL || out_path == NULL || out_cap == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (strcmp(dir_path, "/") == 0) {
        written = snprintf(out_path, out_cap, "/%s", name);
    } else {
        written = snprintf(out_path, out_cap, "%s/%s", dir_path, name);
    }

    if (written <= 0 || (size_t)written >= out_cap) {
        return -(int)M9P_ERR_EMSIZE;
    }

    return 0;
}

static int build_stat_from_info(const char *path,
                                const struct lfs_info *info,
                                struct m9p_stat *out_stat)
{
    const char *name;
    size_t name_len;

    if (path == NULL || info == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(out_stat, 0, sizeof(*out_stat));
    name = node_name_from_path(path);
    name_len = strlen(name);
    if (name_len > M9P_MAX_NAME_LEN) {
        return -(int)M9P_ERR_EMSIZE;
    }

    out_stat->qid.object_id = is_root_path(path) ? 1u : fnv1a32(path);
    out_stat->qid.version = 1u;
    if (info->type == LFS_TYPE_DIR) {
        out_stat->qid.type = M9P_QID_DIR;
        out_stat->perm = M9P_SERVER_PERM_READ;
        out_stat->flags = M9P_STAT_DIR;
        out_stat->size = 0u;
    } else {
        out_stat->qid.type = 0u;
        out_stat->perm = M9P_SERVER_PERM_READ | M9P_SERVER_PERM_WRITE;
        out_stat->flags = 0u;
        out_stat->size = info->size;
    }
    out_stat->mtime = 0u;
    memcpy(out_stat->name, name, name_len);
    out_stat->name[name_len] = '\0';
    return 0;
}

static int stat_path(struct lfs_vfs *vfs,
                     const char *path,
                     struct m9p_stat *out_stat)
{
    struct lfs_info info;
    int rc;

    if (vfs == NULL || vfs->lfs == NULL || path == NULL || out_stat == NULL || path[0] != '/') {
        return -(int)M9P_ERR_EINVAL;
    }

    if (is_root_path(path)) {
        memset(&info, 0, sizeof(info));
        info.type = LFS_TYPE_DIR;
        return build_stat_from_info(path, &info, out_stat);
    }

    memset(&info, 0, sizeof(info));
    rc = lfs_stat(vfs->lfs, path, &info);
    if (rc < 0) {
        return lfs_error_to_m9p(rc);
    }

    return build_stat_from_info(path, &info, out_stat);
}

static int append_dirent(const struct m9p_stat *stat,
                         const char *name,
                         uint32_t *stream_offset,
                         uint32_t read_offset,
                         uint8_t *out_data,
                         uint16_t out_cap,
                         uint16_t *out_count)
{
    uint8_t record[11u + M9P_MAX_NAME_LEN];
    struct m9p_dirent entry;
    size_t name_len;
    size_t record_len;
    size_t start;
    size_t available;

    if (stat == NULL || name == NULL || stream_offset == NULL || out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    name_len = strlen(name);
    if (name_len > M9P_MAX_NAME_LEN) {
        return -(int)M9P_ERR_EMSIZE;
    }

    memset(&entry, 0, sizeof(entry));
    entry.qid = stat->qid;
    entry.perm = stat->perm;
    entry.flags = stat->flags;
    memcpy(entry.name, name, name_len);
    entry.name[name_len] = '\0';
    if (!m9p_build_dirent(&entry, record, sizeof(record), &record_len)) {
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

static int read_dir(struct lfs_vfs *vfs,
                    const char *path,
                    uint32_t offset,
                    uint8_t *out_data,
                    uint16_t out_cap,
                    uint16_t *out_count)
{
    lfs_dir_t dir;
    struct lfs_info info;
    uint32_t stream_offset = 0u;
    int rc;

    if (vfs == NULL || vfs->lfs == NULL || path == NULL || out_count == NULL ||
        (out_cap > 0u && out_data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    rc = lfs_dir_open(vfs->lfs, &dir, path);
    if (rc < 0) {
        return lfs_error_to_m9p(rc);
    }

    while ((rc = lfs_dir_read(vfs->lfs, &dir, &info)) > 0) {
        struct m9p_stat stat;
        char child_path[M9P_MAX_PATH_LEN + 1u];
        int append_rc;

        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        append_rc = join_child_path(path, info.name, child_path, sizeof(child_path));
        if (append_rc != 0) {
            (void)lfs_dir_close(vfs->lfs, &dir);
            return append_rc;
        }

        append_rc = build_stat_from_info(child_path, &info, &stat);
        if (append_rc != 0) {
            (void)lfs_dir_close(vfs->lfs, &dir);
            return append_rc;
        }

        append_rc = append_dirent(&stat,
                                  stat.name,
                                  &stream_offset,
                                  offset,
                                  out_data,
                                  out_cap,
                                  out_count);
        if (append_rc != 0) {
            (void)lfs_dir_close(vfs->lfs, &dir);
            return append_rc < 0 ? append_rc : 0;
        }
    }

    if (rc < 0) {
        (void)lfs_dir_close(vfs->lfs, &dir);
        return lfs_error_to_m9p(rc);
    }

    rc = lfs_dir_close(vfs->lfs, &dir);
    if (rc < 0) {
        return lfs_error_to_m9p(rc);
    }

    return 0;
}

static int read_open_flags(uint8_t mode)
{
    return (mode & 0x03u) == M9P_ORDWR ? LFS_O_RDWR : LFS_O_RDONLY;
}

static int write_open_flags(uint8_t mode)
{
    return (mode & 0x03u) == M9P_ORDWR ? LFS_O_RDWR : LFS_O_WRONLY;
}

static int lfs_stat_cb(void *ctx, const char *path, struct m9p_stat *out_stat)
{
    return stat_path((struct lfs_vfs *)ctx, path, out_stat);
}

static int lfs_open_cb(void *ctx,
                       const char *path,
                       uint8_t mode,
                       struct m9p_qid *out_qid,
                       uint16_t *out_iounit)
{
    struct lfs_vfs *vfs = (struct lfs_vfs *)ctx;
    struct m9p_stat stat;
    uint8_t access = mode & 0x03u;
    int rc;

    if (vfs == NULL || out_qid == NULL || out_iounit == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = stat_path(vfs, path, &stat);
    if (rc != 0) {
        return rc;
    }

    if ((mode & M9P_OTRUNC) != 0u) {
        lfs_file_t file;

        if (access == M9P_OREAD || (stat.flags & M9P_STAT_DIR) != 0u) {
            return -(int)M9P_ERR_EINVAL;
        }

        rc = lfs_file_open(vfs->lfs, &file, path, write_open_flags(mode) | LFS_O_TRUNC);
        if (rc < 0) {
            return lfs_error_to_m9p(rc);
        }

        rc = lfs_file_close(vfs->lfs, &file);
        if (rc < 0) {
            return lfs_error_to_m9p(rc);
        }

        rc = stat_path(vfs, path, &stat);
        if (rc != 0) {
            return rc;
        }
    }

    *out_qid = stat.qid;
    *out_iounit = vfs->iounit;
    return 0;
}

static int lfs_read_cb(void *ctx,
                       const char *path,
                       uint32_t offset,
                       uint8_t mode,
                       uint8_t *out_data,
                       uint16_t out_cap,
                       uint16_t *out_count)
{
    struct lfs_vfs *vfs = (struct lfs_vfs *)ctx;
    struct m9p_stat stat;
    int rc;

    if (vfs == NULL || path == NULL || out_count == NULL || (out_cap > 0u && out_data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    rc = stat_path(vfs, path, &stat);
    if (rc != 0) {
        return rc;
    }

    if ((stat.flags & M9P_STAT_DIR) != 0u) {
        return read_dir(vfs, path, offset, out_data, out_cap, out_count);
    }

    {
        lfs_file_t file;
        lfs_soff_t seek_rc;
        lfs_ssize_t read_rc;

        rc = lfs_file_open(vfs->lfs, &file, path, read_open_flags(mode));
        if (rc < 0) {
            return lfs_error_to_m9p(rc);
        }

        seek_rc = lfs_file_seek(vfs->lfs, &file, (lfs_soff_t)offset, LFS_SEEK_SET);
        if (seek_rc < 0) {
            (void)lfs_file_close(vfs->lfs, &file);
            return lfs_error_to_m9p((int)seek_rc);
        }

        read_rc = lfs_file_read(vfs->lfs, &file, out_data, out_cap);
        if (read_rc < 0) {
            (void)lfs_file_close(vfs->lfs, &file);
            return lfs_error_to_m9p((int)read_rc);
        }

        *out_count = (uint16_t)read_rc;
        rc = lfs_file_close(vfs->lfs, &file);
        if (rc < 0) {
            return lfs_error_to_m9p(rc);
        }
    }

    return 0;
}

static int lfs_write_cb(void *ctx,
                        const char *path,
                        uint32_t offset,
                        uint8_t mode,
                        const uint8_t *data,
                        uint16_t count,
                        uint16_t *out_count)
{
    struct lfs_vfs *vfs = (struct lfs_vfs *)ctx;
    lfs_file_t file;
    lfs_soff_t seek_rc;
    lfs_ssize_t write_rc;
    int rc;

    if (vfs == NULL || path == NULL || out_count == NULL || (count > 0u && data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    rc = lfs_file_open(vfs->lfs, &file, path, write_open_flags(mode));
    if (rc < 0) {
        return lfs_error_to_m9p(rc);
    }

    seek_rc = lfs_file_seek(vfs->lfs, &file, (lfs_soff_t)offset, LFS_SEEK_SET);
    if (seek_rc < 0) {
        (void)lfs_file_close(vfs->lfs, &file);
        return lfs_error_to_m9p((int)seek_rc);
    }

    write_rc = lfs_file_write(vfs->lfs, &file, data, count);
    if (write_rc < 0) {
        (void)lfs_file_close(vfs->lfs, &file);
        return lfs_error_to_m9p((int)write_rc);
    }

    *out_count = (uint16_t)write_rc;
    rc = lfs_file_sync(vfs->lfs, &file);
    if (rc < 0) {
        (void)lfs_file_close(vfs->lfs, &file);
        return lfs_error_to_m9p(rc);
    }

    rc = lfs_file_close(vfs->lfs, &file);
    if (rc < 0) {
        return lfs_error_to_m9p(rc);
    }

    return 0;
}

static int lfs_clunk_cb(void *ctx, const char *path, bool was_open)
{
    (void)ctx;
    (void)path;
    (void)was_open;
    return 0;
}

static const struct m9p_server_ops k_lfs_vfs_ops = {
    lfs_stat_cb,
    lfs_open_cb,
    lfs_read_cb,
    lfs_write_cb,
    lfs_clunk_cb,
};

void lfs_vfs_get_default_config(struct lfs_vfs_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    out_config->iounit = LFS_VFS_DEFAULT_IOUNIT;
}

int lfs_vfs_init(struct lfs_vfs *vfs, const struct lfs_vfs_config *config)
{
    struct lfs_vfs_config default_config;
    const struct lfs_vfs_config *active_config = config;
    int rc;

    if (vfs == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (active_config == NULL) {
        lfs_vfs_get_default_config(&default_config);
        active_config = &default_config;
    }

    rc = lfs_port_init();
    if (rc != 0) {
        return -(int)M9P_ERR_EIO;
    }

    vfs->lfs = lfs_port_fs();
    if (vfs->lfs == NULL) {
        return -(int)M9P_ERR_EIO;
    }

    vfs->iounit = active_config->iounit == 0u ? LFS_VFS_DEFAULT_IOUNIT : active_config->iounit;
    return 0;
}

lfs_t *lfs_vfs_fs(struct lfs_vfs *vfs)
{
    return vfs != NULL ? vfs->lfs : NULL;
}

const struct m9p_server_ops *lfs_vfs_ops(void)
{
    return &k_lfs_vfs_ops;
}