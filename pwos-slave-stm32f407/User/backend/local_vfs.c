/**
 * @file local_vfs.c
 * @brief 面向 Mini9P 的极简虚拟节点本地 VFS。
 *
 * 本模块是 STM32 Mini9P 服务器使用的首个后端实现。它有意只暴露一个
 * 极小的只读虚拟树：
 *
 * - /
 * - /sys
 * - /sys/health
 *
 * 服务器仍然负责 fid/会话语义。local_vfs 仅通过 m9p_server_ops 回答
 * 基于路径的资源回调。
 */

#include "local_vfs.h"

#include <stddef.h>
#include <string.h>

/**
 * @brief 固定虚拟节点描述。
 */
struct local_vfs_node {
    const char *path;        /**< 节点本地绝对路径。 */
    const char *content;     /**< 普通虚拟文件的内容；目录则为 NULL。 */
    struct m9p_stat stat;    /**< stat/open 返回的 Mini9P 元数据。 */
};

/**
 * @brief local_vfs v1 导出的静态虚拟命名空间。
 */
static const struct local_vfs_node k_nodes[] = {
    {
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
        "ok\n",
        {{M9P_QID_VIRTUAL | M9P_QID_READONLY, 0u, 1u, 3u},
         M9P_SERVER_PERM_READ,
         M9P_STAT_VIRTUAL,
         3u,
         0u,
         "health"},
    },
};

/**
 * @brief 按绝对路径查找虚拟节点。
 *
 * @param[in] path 节点本地绝对路径。
 * @return 匹配的节点，未找到时返回 NULL。
 */
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

/**
 * @brief 查询虚拟节点的元数据。
 *
 * @param[in] path 节点本地绝对路径。
 * @param[out] out_stat 输出元数据。
 * @return 成功返回 0，失败返回负的 Mini9P 错误码。
 */
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

/**
 * @brief 校验 local_vfs v1 唯一支持的打开模式。
 *
 * @param[in] mode Mini9P 打开模式。
 * @return M9P_OREAD 返回 0，否则返回 -M9P_ERR_ENOTSUP。
 */
static int validate_read_open_mode(uint8_t mode)
{
    return mode == M9P_OREAD ? 0 : -(int)M9P_ERR_ENOTSUP;
}

/**
 * @brief 从绝对路径派生目录项名称。
 *
 * @param[in] path 节点本地绝对路径。
 * @return path 的最后一段；根路径返回 "/"。
 */
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

/**
 * @brief 将一条目录条目追加到流式 Mini9P 目录读取中。
 *
 * 目录读取是字节流。read_offset 可能指向某条目录项的中间，因此本辅助
 * 函数在维护逻辑流偏移的同时，只将请求可见的范围复制到 out_data。
 *
 * @param[in] node 要编码的子节点。
 * @param[in,out] stream_offset 完整目录流中已消耗的逻辑字节数。
 * @param[in] read_offset 请求的 Tread 偏移量。
 * @param[out] out_data 输出数据缓冲区。
 * @param[in] out_cap 输出数据容量。
 * @param[in,out] out_count 已发出/返回后发出的字节数。
 * @return 0 表示继续，正值表示输出已满，负值为 Mini9P 错误码。
 */
static int append_dirent(const struct local_vfs_node *node,
                         uint32_t *stream_offset,
                         uint32_t read_offset,
                         uint8_t *out_data,
                         uint16_t out_cap,
                         uint16_t *out_count)
{
    uint8_t record[11u + M9P_MAX_NAME_LEN];
    struct m9p_dirent entry;
    const char *name;
    size_t name_len;
    size_t record_len;
    size_t start;
    size_t available;

    if (node == NULL || stream_offset == NULL || out_count == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    name = node_name_from_path(node->path);
    name_len = strlen(name);
    if (name_len > M9P_MAX_NAME_LEN) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(&entry, 0, sizeof(entry));
    entry.qid = node->stat.qid;
    entry.perm = node->stat.perm;
    entry.flags = node->stat.flags;
    memcpy(entry.name, name, name_len);
    entry.name[name_len] = '\0';
    if (!m9p_build_dirent(&entry, record, sizeof(record), &record_len)) {
        return -(int)M9P_ERR_EINVAL;
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

/**
 * @brief 判断 child_path 是否是 dir_path 的直接子项。
 *
 * @param[in] dir_path 目录路径。
 * @param[in] child_path 候选子路径。
 * @return child_path 直接在 dir_path 下时返回 true。
 */
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

/**
 * @brief 将虚拟目录读取为 Mini9P 目录项字节流。
 *
 * @param[in] path 目录路径。
 * @param[in] offset 请求的 Tread 偏移量。
 * @param[out] out_data 输出数据缓冲区。
 * @param[in] out_cap 输出数据容量。
 * @param[out] out_count 发出的字节数。
 * @return 成功返回 0，失败返回负的 Mini9P 错误码。
 */
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

/**
 * @brief m9p_server_ops::stat 的实现。
 */
static int local_stat(void *ctx, const char *path, struct m9p_stat *out_stat)
{
    (void)ctx;
    return stat_node(path, out_stat);
}

/**
 * @brief m9p_server_ops::open 的实现。
 *
 * local_vfs v1 没有真正的打开资源；open 校验路径/模式并返回节点 qid
 * 加上配置的 iounit。
 */
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

/**
 * @brief m9p_server_ops::read 的实现。
 */
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

/**
 * @brief m9p_server_ops::write 的实现。
 *
 * local_vfs v1 为只读。
 */
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

/**
 * @brief m9p_server_ops::clunk 的实现。
 *
 * local_vfs v1 不持有按打开实例的资源，因此 clunk 仅校验路径。
 */
static int local_clunk(void *ctx, const char *path, bool was_open)
{
    (void)ctx;
    (void)was_open;

    if (path == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    return 0;
}

/**
 * @brief mini9p_server 使用的静态回调表。
 */
static const struct m9p_server_ops k_local_vfs_ops = {
    local_stat,
    local_open,
    local_read,
    local_write,
    local_clunk,
};

/**
 * @brief 用默认值填充 local_vfs_config。
 *
 * @param[out] out_config 目标配置。NULL 将被忽略。
 */
void local_vfs_get_default_config(struct local_vfs_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    out_config->iounit = LOCAL_VFS_DEFAULT_IOUNIT;
}

/**
 * @brief 初始化 local_vfs 实例。
 *
 * @param[out] vfs 要初始化的实例。
 * @param[in] config 可选配置。NULL 表示使用默认值。
 * @return 成功返回 0，失败返回负的 Mini9P 错误码。
 */
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

/**
 * @brief 返回 local_vfs 的 Mini9P 服务器回调表。
 *
 * @return 指向静态 m9p_server_ops 表的指针。
 */
const struct m9p_server_ops *local_vfs_ops(void)
{
    return &k_local_vfs_ops;
}
