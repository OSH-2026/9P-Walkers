/**
 * @file local_vfs.c
 * @brief Minimal virtual-node local VFS for Mini9P.
 */

#include "local_vfs.h"

#include <stddef.h>
#include <string.h>

struct local_vfs_node {
    const char *path;
    const char *name;
    const char *content;
    struct m9p_stat stat;
};

static const struct local_vfs_node k_nodes[] = {
    {
        "/",
        "/",
        NULL,
        {{M9P_QID_DIR | M9P_QID_READONLY, 0u, 1u, 1u},
         M9P_SERVER_PERM_READ,
         M9P_STAT_DIR | M9P_STAT_VIRTUAL,
         0u,
         0u,
         "/"},
    },
    {
        "/sys",
        "sys",
        NULL,
        {{M9P_QID_DIR | M9P_QID_VIRTUAL | M9P_QID_READONLY, 0u, 1u, 2u},
         M9P_SERVER_PERM_READ,
         M9P_STAT_DIR | M9P_STAT_VIRTUAL,
         0u,
         0u,
         "sys"},
    },
    {
        "/sys/health",
        "health",
        "ok\n",
        {{M9P_QID_VIRTUAL | M9P_QID_READONLY, 0u, 1u, 3u},
         M9P_SERVER_PERM_READ,
         M9P_STAT_VIRTUAL,
         3u,
         0u,
         "health"},
    },
};

static const struct local_vfs_node *find_node(const char *path)
{
    size_t i;

    for (i = 0u; i < sizeof(k_nodes) / sizeof(k_nodes[0]); ++i) {
        if (strcmp(k_nodes[i].path, path) == 0) {
            return &k_nodes[i];
        }
    }

    return NULL;
}

static int stat_node(const char *path, struct m9p_stat *out_stat)
{
    const struct local_vfs_node *node;

    if (path == NULL || out_stat == NULL || path[0] != '/') {
        return -(int)M9P_ERR_EINVAL;
    }

    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }

    *out_stat = node->stat;
    return 0;
}

static int validate_read_open_mode(uint8_t mode)
{
    return mode == M9P_OREAD ? 0 : -(int)M9P_ERR_ENOTSUP;
}

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8) & 0xffu);
    dst[2] = (uint8_t)((value >> 16) & 0xffu);
    dst[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void encode_qid(uint8_t *dst, const struct m9p_qid *qid)
{
    dst[0] = qid->type;
    dst[1] = qid->reserved;
    put_le16(dst + 2u, qid->version);
    put_le32(dst + 4u, qid->object_id);
}

static int append_dirent(const struct local_vfs_node *node,
                         uint32_t *stream_offset,
                         uint32_t read_offset,
                         uint8_t *out_data,
                         uint16_t out_cap,
                         uint16_t *out_count)
{
    uint8_t record[11u + M9P_MAX_NAME_LEN];
    size_t name_len;
    size_t record_len;
    size_t start;
    size_t available;

    if (node == NULL || stream_offset == NULL || out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    name_len = strlen(node->name);
    if (name_len > M9P_MAX_NAME_LEN) {
        name_len = M9P_MAX_NAME_LEN;
    }

    encode_qid(record, &node->stat.qid);
    record[8] = node->stat.perm;
    record[9] = node->stat.flags;
    record[10] = (uint8_t)name_len;
    memcpy(record + 11u, node->name, name_len);
    record_len = 11u + name_len;

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

static bool is_direct_child(const char *dir_path, const char *child_path)
{
    const char *rest;

    if (dir_path == NULL || child_path == NULL || strcmp(dir_path, child_path) == 0) {
        return false;
    }

    if (strcmp(dir_path, "/") == 0) {
        if (child_path[0] != '/') {
            return false;
        }
        rest = child_path + 1u;
    } else {
        size_t dir_len = strlen(dir_path);

        if (strncmp(child_path, dir_path, dir_len) != 0 || child_path[dir_len] != '/') {
            return false;
        }
        rest = child_path + dir_len + 1u;
    }

    return rest[0] != '\0' && strchr(rest, '/') == NULL;
}

static int read_dir(const char *path,
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
    for (i = 0u; i < sizeof(k_nodes) / sizeof(k_nodes[0]); ++i) {
        int rc;

        if (!is_direct_child(path, k_nodes[i].path)) {
            continue;
        }

        rc = append_dirent(&k_nodes[i], &stream_offset, offset, out_data, out_cap, out_count);
        if (rc != 0) {
            return rc < 0 ? rc : 0;
        }
    }

    return 0;
}

static int local_stat(void *ctx, const char *path, struct m9p_stat *out_stat)
{
    (void)ctx;
    return stat_node(path, out_stat);
}

static int local_open(void *ctx,
                      const char *path,
                      uint8_t mode,
                      struct m9p_qid *out_qid,
                      uint16_t *out_iounit)
{
    struct local_vfs *vfs = (struct local_vfs *)ctx;
    struct m9p_stat stat;
    int rc;

    if (vfs == NULL || out_qid == NULL || out_iounit == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = validate_read_open_mode(mode);
    if (rc != 0) {
        return rc;
    }

    rc = stat_node(path, &stat);
    if (rc != 0) {
        return rc;
    }

    *out_qid = stat.qid;
    *out_iounit = vfs->iounit;
    return 0;
}

static int local_read(void *ctx,
                      const char *path,
                      uint32_t offset,
                      uint8_t mode,
                      uint8_t *out_data,
                      uint16_t out_cap,
                      uint16_t *out_count)
{
    const struct local_vfs_node *node;

    (void)ctx;
    (void)mode;
    if (path == NULL || out_count == NULL || (out_cap > 0u && out_data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }

    if ((node->stat.flags & M9P_STAT_DIR) != 0u) {
        return read_dir(path, offset, out_data, out_cap, out_count);
    }
    if (node->content == NULL) {
        return -(int)M9P_ERR_EIO;
    }

    {
        size_t len = strlen(node->content);
        size_t count;

        if (offset >= len) {
            return 0;
        }

        count = len - (size_t)offset;
        if (count > out_cap) {
            count = out_cap;
        }

        memcpy(out_data, node->content + offset, count);
        *out_count = (uint16_t)count;
    }

    return 0;
}

static int local_write(void *ctx,
                       const char *path,
                       uint32_t offset,
                       uint8_t mode,
                       const uint8_t *data,
                       uint16_t count,
                       uint16_t *out_count)
{
    (void)ctx;
    (void)path;
    (void)offset;
    (void)mode;
    (void)data;
    (void)count;
    (void)out_count;
    return -(int)M9P_ERR_ENOTSUP;
}

static int local_clunk(void *ctx, const char *path, bool was_open)
{
    (void)ctx;
    (void)was_open;

    if (path == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    return 0;
}

static const struct m9p_server_ops k_local_vfs_ops = {
    local_stat,
    local_open,
    local_read,
    local_write,
    local_clunk,
};

void local_vfs_get_default_config(struct local_vfs_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    out_config->iounit = LOCAL_VFS_DEFAULT_IOUNIT;
}

int local_vfs_init(struct local_vfs *vfs, const struct local_vfs_config *config)
{
    struct local_vfs_config default_config;
    const struct local_vfs_config *active_config = config;

    if (vfs == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (active_config == NULL) {
        local_vfs_get_default_config(&default_config);
        active_config = &default_config;
    }

    vfs->iounit = active_config->iounit == 0u ? LOCAL_VFS_DEFAULT_IOUNIT : active_config->iounit;
    return 0;
}

const struct m9p_server_ops *local_vfs_ops(void)
{
    return &k_local_vfs_ops;
}
