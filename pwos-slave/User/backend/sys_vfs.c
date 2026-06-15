/**
 * @file sys_vfs.c
 * @brief Mini9P backend for /sys virtual files.
 */

#include "sys_vfs.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define SYS_VFS_HEALTH_TEXT "ok\n"
#define SYS_VFS_ROUTES_TEXT_CAP 256u
#define SYS_VFS_UART_TEXT_CAP 1024u
#define SYS_VFS_LOG_TEXT_CAP 4096u

enum sys_vfs_node_kind {
    SYS_VFS_NODE_DIR = 0,
    SYS_VFS_NODE_HEALTH,
    SYS_VFS_NODE_INFO,
    SYS_VFS_NODE_ROUTES,
    SYS_VFS_NODE_UART,
    SYS_VFS_NODE_LOG,
};

struct sys_vfs_node {
    const char *path;
    const char *name;
    enum sys_vfs_node_kind kind;
    uint8_t qid_type;
    uint8_t flags;
    uint8_t perm;
    uint32_t object_id;
};

static const struct sys_vfs_node k_sys_nodes[] = {
    {"/sys", "sys", SYS_VFS_NODE_DIR, M9P_QID_DIR | M9P_QID_VIRTUAL, M9P_STAT_DIR | M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 0x100u},
    {"/sys/health", "health", SYS_VFS_NODE_HEALTH, M9P_QID_VIRTUAL | M9P_QID_READONLY, M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 0x101u},
    {"/sys/info", "info", SYS_VFS_NODE_INFO, M9P_QID_VIRTUAL | M9P_QID_READONLY, M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 0x102u},
    {"/sys/routes", "routes", SYS_VFS_NODE_ROUTES, M9P_QID_VIRTUAL | M9P_QID_READONLY, M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 0x103u},
    {"/sys/uart", "uart", SYS_VFS_NODE_UART, M9P_QID_VIRTUAL | M9P_QID_READONLY, M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 0x104u},
    {"/sys/log", "log", SYS_VFS_NODE_LOG, M9P_QID_VIRTUAL | M9P_QID_READONLY, M9P_STAT_VIRTUAL, M9P_SERVER_PERM_READ, 0x105u},
};

static const struct sys_vfs_node *find_node(const char *path)
{
    size_t i;

    for (i = 0u; i < sizeof(k_sys_nodes) / sizeof(k_sys_nodes[0]); ++i) {
        if (strcmp(path, k_sys_nodes[i].path) == 0) {
            return &k_sys_nodes[i];
        }
    }
    return NULL;
}

static void fill_stat(const struct sys_vfs_node *node, uint32_t size, struct m9p_stat *out_stat)
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

static int append_dirent(const struct sys_vfs_node *node,
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
    for (i = 0u; i < sizeof(k_sys_nodes) / sizeof(k_sys_nodes[0]); ++i) {
        int rc;

        if (strcmp(k_sys_nodes[i].path, "/sys") == 0) {
            continue;
        }

        rc = append_dirent(&k_sys_nodes[i], &stream_offset, offset, out_data, out_cap, out_count);
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

static int build_routes_text(struct sys_vfs *vfs, char *out, size_t out_cap)
{
    int rc;

    if (vfs == NULL || out == NULL || out_cap == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (vfs->routes_text_fn == NULL) {
        (void)snprintf(out, out_cap, "routes unavailable\n");
        return 0;
    }

    rc = vfs->routes_text_fn(vfs->routes_text_ctx, out, out_cap);
    if (rc != 0) {
        (void)snprintf(out, out_cap, "routes error=%d\n", rc);
    }
    return 0;
}

static int build_log_text(struct sys_vfs *vfs, char *out, size_t out_cap)
{
    int rc;

    if (vfs == NULL || out == NULL || out_cap == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (vfs->log_text_fn == NULL) {
        (void)snprintf(out, out_cap, "log unavailable\n");
        return 0;
    }

    rc = vfs->log_text_fn(vfs->log_text_ctx, out, out_cap);
    if (rc != 0) {
        (void)snprintf(out, out_cap, "log error=%d\n", rc);
    }
    return 0;
}

static int build_uart_text(struct sys_vfs *vfs, char *out, size_t out_cap)
{
    int rc;

    if (vfs == NULL || out == NULL || out_cap == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (vfs->uart_text_fn == NULL) {
        (void)snprintf(out, out_cap, "uart unavailable\n");
        return 0;
    }

    rc = vfs->uart_text_fn(vfs->uart_text_ctx, out, out_cap);
    if (rc != 0) {
        (void)snprintf(out, out_cap, "uart error=%d\n", rc);
    }
    return 0;
}

static int sys_stat_cb(void *ctx, const char *path, struct m9p_stat *out_stat)
{
    struct sys_vfs *vfs = (struct sys_vfs *)ctx;
    const struct sys_vfs_node *node;
    uint32_t size = 0u;

    if (vfs == NULL || path == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }

    if (node->kind == SYS_VFS_NODE_HEALTH) {
        size = (uint32_t)strlen(SYS_VFS_HEALTH_TEXT);
    } else if (node->kind == SYS_VFS_NODE_INFO && vfs->info_text != NULL) {
        size = (uint32_t)strlen(vfs->info_text);
    } else if (node->kind == SYS_VFS_NODE_ROUTES ||
               node->kind == SYS_VFS_NODE_UART ||
               node->kind == SYS_VFS_NODE_LOG) {
        size = vfs->iounit;
    }

    fill_stat(node, size, out_stat);
    return 0;
}

static int sys_open_cb(void *ctx,
                       const char *path,
                       uint8_t mode,
                       struct m9p_qid *out_qid,
                       uint16_t *out_iounit)
{
    struct sys_vfs *vfs = (struct sys_vfs *)ctx;
    struct m9p_stat stat;
    int rc;

    if (vfs == NULL || out_qid == NULL || out_iounit == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if ((mode & 0x03u) != M9P_OREAD || (mode & M9P_OTRUNC) != 0u) {
        return -(int)M9P_ERR_EPERM;
    }

    rc = sys_stat_cb(ctx, path, &stat);
    if (rc != 0) {
        return rc;
    }

    *out_qid = stat.qid;
    *out_iounit = vfs->iounit;
    return 0;
}

static int sys_read_cb(void *ctx,
                       const char *path,
                       uint32_t offset,
                       uint8_t mode,
                       uint8_t *out_data,
                       uint16_t out_cap,
                       uint16_t *out_count)
{
    struct sys_vfs *vfs = (struct sys_vfs *)ctx;
    const struct sys_vfs_node *node;

    (void)mode;
    if (vfs == NULL || path == NULL || out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    node = find_node(path);
    if (node == NULL) {
        return -(int)M9P_ERR_ENOENT;
    }

    if (node->kind == SYS_VFS_NODE_DIR) {
        return read_dir(offset, out_data, out_cap, out_count);
    }
    if (node->kind == SYS_VFS_NODE_HEALTH) {
        return copy_text(SYS_VFS_HEALTH_TEXT, offset, out_data, out_cap, out_count);
    }
    if (node->kind == SYS_VFS_NODE_INFO) {
        return copy_text(vfs->info_text != NULL ? vfs->info_text : "", offset, out_data, out_cap, out_count);
    }
    if (node->kind == SYS_VFS_NODE_ROUTES) {
        char routes[SYS_VFS_ROUTES_TEXT_CAP];
        int rc = build_routes_text(vfs, routes, sizeof(routes));

        if (rc != 0) {
            return rc;
        }
        return copy_text(routes, offset, out_data, out_cap, out_count);
    }
    if (node->kind == SYS_VFS_NODE_UART) {
        char uart_text[SYS_VFS_UART_TEXT_CAP];
        int rc = build_uart_text(vfs, uart_text, sizeof(uart_text));

        if (rc != 0) {
            return rc;
        }
        return copy_text(uart_text, offset, out_data, out_cap, out_count);
    }
    {
        char log_text[SYS_VFS_LOG_TEXT_CAP];
        int rc = build_log_text(vfs, log_text, sizeof(log_text));

        if (rc != 0) {
            return rc;
        }
        return copy_text(log_text, offset, out_data, out_cap, out_count);
    }
}

static int sys_write_cb(void *ctx,
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
    if (out_count != NULL) {
        *out_count = 0u;
    }
    return -(int)M9P_ERR_EPERM;
}

static int sys_clunk_cb(void *ctx, const char *path, bool was_open)
{
    (void)ctx;
    (void)path;
    (void)was_open;
    return 0;
}

static const struct m9p_server_ops k_sys_vfs_ops = {
    sys_stat_cb,
    sys_open_cb,
    sys_read_cb,
    sys_write_cb,
    sys_clunk_cb,
};

int sys_vfs_init(struct sys_vfs *vfs, const struct sys_vfs_config *config)
{
    if (vfs == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(vfs, 0, sizeof(*vfs));
    if (config != NULL) {
        vfs->info_text = config->info_text;
        vfs->routes_text_fn = config->routes_text_fn;
        vfs->routes_text_ctx = config->routes_text_ctx;
        vfs->log_text_fn = config->log_text_fn;
        vfs->log_text_ctx = config->log_text_ctx;
        vfs->uart_text_fn = config->uart_text_fn;
        vfs->uart_text_ctx = config->uart_text_ctx;
        vfs->iounit = config->iounit == 0u ? SYS_VFS_DEFAULT_IOUNIT : config->iounit;
    } else {
        vfs->iounit = SYS_VFS_DEFAULT_IOUNIT;
    }
    return 0;
}

const struct m9p_server_ops *sys_vfs_ops(void)
{
    return &k_sys_vfs_ops;
}
