/**
 * @file dev_vfs.c
 * @brief Mini9P backend for /dev virtual files.
 */

#include "dev_vfs.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define DEV_VFS_LED_UNCONFIGURED_TEXT "unconfigured\n"

enum dev_vfs_node_kind {
    DEV_VFS_NODE_DIR = 0,
    DEV_VFS_NODE_LED,
};

struct dev_vfs_node {
    const char *path;
    const char *name;
    enum dev_vfs_node_kind kind;
    uint8_t qid_type;
    uint8_t flags;
    uint8_t perm;
    uint32_t object_id;
};

static const struct dev_vfs_node k_dev_nodes[] = {
    {"/dev", "dev", DEV_VFS_NODE_DIR, M9P_QID_DIR | M9P_QID_VIRTUAL, M9P_STAT_DIR | M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 0x200u},
    {"/dev/led", "led", DEV_VFS_NODE_LED, M9P_QID_DEVICE, M9P_STAT_DEVICE, M9P_SERVER_PERM_READ | M9P_SERVER_PERM_WRITE, 0x201u},
};

static const struct dev_vfs_node *find_node(const char *path)
{
    size_t i;

    for (i = 0u; i < sizeof(k_dev_nodes) / sizeof(k_dev_nodes[0]); ++i) {
        if (strcmp(path, k_dev_nodes[i].path) == 0) {
            return &k_dev_nodes[i];
        }
    }
    return NULL;
}

static void fill_stat(const struct dev_vfs_node *node, uint32_t size, struct m9p_stat *out_stat)
{
    size_t name_len;

    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->qid.type = node->qid_type;
    out_stat->qid.version = 1u;
    out_stat->qid.object_id = node->object_id;
    out_stat->perm = node->perm;
    out_stat->flags = node->flags;
    out_stat->size = size;

    name_len = strlen(node->name);
    if (name_len > M9P_MAX_NAME_LEN) {
        name_len = M9P_MAX_NAME_LEN;
    }
    memcpy(out_stat->name, node->name, name_len);
    out_stat->name[name_len] = '\0';
}

static int append_dirent(const struct dev_vfs_node *node,
                         uint32_t *stream_offset,
                         uint32_t read_offset,
                         uint8_t *out_data,
                         uint16_t out_cap,
                         uint16_t *out_count)
{
    uint8_t record[11u + M9P_MAX_NAME_LEN];
    struct m9p_dirent entry;
    size_t record_len;
    size_t start;
    size_t available;

    memset(&entry, 0, sizeof(entry));
    entry.qid.type = node->qid_type;
    entry.qid.version = 1u;
    entry.qid.object_id = node->object_id;
    entry.perm = node->perm;
    entry.flags = node->flags;
    (void)snprintf(entry.name, sizeof(entry.name), "%s", node->name);
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

static int read_dir(uint32_t offset, uint8_t *out_data, uint16_t out_cap, uint16_t *out_count)
{
    uint32_t stream_offset = 0u;
    size_t i;

    if (out_count == NULL || (out_cap > 0u && out_data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    for (i = 0u; i < sizeof(k_dev_nodes) / sizeof(k_dev_nodes[0]); ++i) {
        int rc;

        if (strcmp(k_dev_nodes[i].path, "/dev") == 0) {
            continue;
        }

        rc = append_dirent(&k_dev_nodes[i], &stream_offset, offset, out_data, out_cap, out_count);
        if (rc != 0) {
            return rc > 0 ? 0 : rc;
        }
    }
    return 0;
}

static int copy_text(const char *text,
                     uint32_t offset,
                     uint8_t *out_data,
                     uint16_t out_cap,
                     uint16_t *out_count)
{
    size_t len;
    size_t available;

    if (text == NULL || out_count == NULL || (out_cap > 0u && out_data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    len = strlen(text);
    if (offset >= len) {
        return 0;
    }

    available = len - offset;
    if (available > out_cap) {
        available = out_cap;
    }

    memcpy(out_data, text + offset, available);
    *out_count = (uint16_t)available;
    return 0;
}

static int dev_stat_cb(void *ctx, const char *path, struct m9p_stat *out_stat)
{
    (void)ctx;
    const struct dev_vfs_node *node;
    uint32_t size = 0u;

    if (path == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }
    if (node->kind == DEV_VFS_NODE_LED) {
        size = (uint32_t)strlen(DEV_VFS_LED_UNCONFIGURED_TEXT);
    }

    fill_stat(node, size, out_stat);
    return 0;
}

static int dev_open_cb(void *ctx,
                       const char *path,
                       uint8_t mode,
                       struct m9p_qid *out_qid,
                       uint16_t *out_iounit)
{
    struct dev_vfs *vfs = (struct dev_vfs *)ctx;
    struct m9p_stat stat;
    int rc;

    if (vfs == NULL || out_qid == NULL || out_iounit == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if ((mode & M9P_OTRUNC) != 0u) {
        return -(int)M9P_ERR_ENOTSUP;
    }

    rc = dev_stat_cb(ctx, path, &stat);
    if (rc != 0) {
        return rc;
    }
    if ((stat.flags & M9P_STAT_DIR) != 0u && (mode & 0x03u) != M9P_OREAD) {
        return -(int)M9P_ERR_EISDIR;
    }

    *out_qid = stat.qid;
    *out_iounit = vfs->iounit;
    return 0;
}

static int dev_read_cb(void *ctx,
                       const char *path,
                       uint32_t offset,
                       uint8_t mode,
                       uint8_t *out_data,
                       uint16_t out_cap,
                       uint16_t *out_count)
{
    struct dev_vfs *vfs = (struct dev_vfs *)ctx;
    const struct dev_vfs_node *node;

    (void)mode;
    if (vfs == NULL || path == NULL || out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }
    if (node->kind == DEV_VFS_NODE_DIR) {
        return read_dir(offset, out_data, out_cap, out_count);
    }

    if (vfs->device_ops != NULL && vfs->device_ops->read_led != NULL) {
        char text[32];
        uint16_t len = 0u;
        int rc = vfs->device_ops->read_led(vfs->device_ctx, text, sizeof(text), &len);

        if (rc != 0) {
            return rc;
        }
        if (len >= sizeof(text)) {
            len = sizeof(text) - 1u;
        }
        text[len] = '\0';
        return copy_text(text, offset, out_data, out_cap, out_count);
    }

    return copy_text(DEV_VFS_LED_UNCONFIGURED_TEXT, offset, out_data, out_cap, out_count);
}

static int dev_write_cb(void *ctx,
                        const char *path,
                        uint32_t offset,
                        uint8_t mode,
                        const uint8_t *data,
                        uint16_t count,
                        uint16_t *out_count)
{
    struct dev_vfs *vfs = (struct dev_vfs *)ctx;
    const struct dev_vfs_node *node;

    (void)mode;
    if (vfs == NULL || path == NULL || out_count == NULL || (count > 0u && data == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }
    if (node->kind != DEV_VFS_NODE_LED) {
        return -(int)M9P_ERR_EPERM;
    }
    if (offset != 0u) {
        return -(int)M9P_ERR_EOFFS;
    }
    if (vfs->device_ops == NULL || vfs->device_ops->write_led == NULL) {
        return -(int)M9P_ERR_ENOTSUP;
    }

    return vfs->device_ops->write_led(vfs->device_ctx, data, count, out_count);
}

static int dev_clunk_cb(void *ctx, const char *path, bool was_open)
{
    (void)ctx;
    (void)path;
    (void)was_open;
    return 0;
}

static const struct m9p_server_ops k_dev_vfs_ops = {
    dev_stat_cb,
    dev_open_cb,
    dev_read_cb,
    dev_write_cb,
    dev_clunk_cb,
};

int dev_vfs_init(struct dev_vfs *vfs, const struct dev_vfs_config *config)
{
    if (vfs == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(vfs, 0, sizeof(*vfs));
    if (config != NULL) {
        vfs->device_ops = config->device_ops;
        vfs->device_ctx = config->device_ctx;
        vfs->iounit = config->iounit == 0u ? DEV_VFS_DEFAULT_IOUNIT : config->iounit;
    } else {
        vfs->iounit = DEV_VFS_DEFAULT_IOUNIT;
    }
    return 0;
}

const struct m9p_server_ops *dev_vfs_ops(void)
{
    return &k_dev_vfs_ops;
}
