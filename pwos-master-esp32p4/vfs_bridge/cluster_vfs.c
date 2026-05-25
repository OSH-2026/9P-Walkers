#include "cluster_vfs.h"

#include <stdio.h>
#include <string.h>

#define CLUSTER_VFS_MAX_NAME 16
#define CLUSTER_VFS_UNASSIGNED_ADDR MESH_ADDR_UNASSIGNED

enum cluster_vfs_m9p_state {
    CLUSTER_VFS_M9P_EMPTY = 0,
    CLUSTER_VFS_M9P_NEW,
    CLUSTER_VFS_M9P_ATTACHED,
};

struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];
    struct m9p_client *client;
    uint8_t mesh_addr;
    uint8_t hw_uid[CLUSTER_VFS_UID_LEN];
    bool has_hw_uid;
    enum cluster_vfs_m9p_state m9p_state;
};

struct cluster_vfs_file {
    bool used;
    struct cluster_vfs_route *route;
    uint16_t remote_fid;
    struct m9p_qid qid;
    uint8_t mode;
    uint32_t offset;
};

static struct cluster_vfs_route g_routes[CLUSTER_VFS_MAX_ROUTES];
static struct cluster_vfs_file g_open_files[CLUSTER_VFS_MAX_OPEN];
static struct cluster *g_mesh_cluster;

static void reset_route(struct cluster_vfs_route *route)
{
    if (route == NULL) {
        return;
    }

    memset(route, 0, sizeof(*route));
    route->mesh_addr = CLUSTER_VFS_UNASSIGNED_ADDR;
    route->m9p_state = CLUSTER_VFS_M9P_EMPTY;
}

static void reset_open_file(struct cluster_vfs_file *file)
{
    if (file == NULL) {
        return;
    }

    memset(file, 0, sizeof(*file));
}

static bool route_is_reachable(const struct cluster_vfs_route *route)
{
    if (route == NULL) {
        return false;
    }

    if (route->m9p_state == CLUSTER_VFS_M9P_EMPTY) {
        return false;
    }

    if (route->has_hw_uid && route->mesh_addr == CLUSTER_VFS_UNASSIGNED_ADDR) {
        return false;
    }

    if (route->has_hw_uid && g_mesh_cluster != NULL) {
        bool reachable = false;
        if (cluster_can_reach(g_mesh_cluster, route->mesh_addr, &reachable) != 0 || !reachable) {
            return false;
        }
    }

    return true;
}

static void reset_client_session(struct m9p_client *client)
{
    if (client == NULL) {
        return;
    }

    m9p_client_reset_session(client);
}

static struct cluster_vfs_route *alloc_route(void)
{
    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].m9p_state == CLUSTER_VFS_M9P_EMPTY)
        {
            return &g_routes[i];
        }
    }
    return NULL;
}

static struct cluster_vfs_file *alloc_open_file(void)
{
    for (size_t i = 0; i < CLUSTER_VFS_MAX_OPEN; ++i)
    {
        if (!g_open_files[i].used)
        {
            return &g_open_files[i];
        }
    }
    return NULL;
}

static struct cluster_vfs_route *find_route(const char *target)
{
    if (!target)
    {
        return NULL;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].m9p_state != CLUSTER_VFS_M9P_EMPTY &&
            strcmp(g_routes[i].target, target) == 0)
        {
            return &g_routes[i];
        }
    }

    return NULL;
}

static struct cluster_vfs_route *find_route_by_uid(const uint8_t hw_uid[CLUSTER_VFS_UID_LEN])
{
    if (hw_uid == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].m9p_state == CLUSTER_VFS_M9P_EMPTY || !g_routes[i].has_hw_uid)
        {
            continue;
        }
        if (memcmp(g_routes[i].hw_uid, hw_uid, CLUSTER_VFS_UID_LEN) == 0)
        {
            return &g_routes[i];
        }
    }

    return NULL;
}

static struct cluster_vfs_route *find_route_by_mesh_addr(uint8_t mesh_addr)
{
    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].m9p_state == CLUSTER_VFS_M9P_EMPTY)
        {
            continue;
        }
        if (g_routes[i].mesh_addr == mesh_addr)
        {
            return &g_routes[i];
        }
    }

    return NULL;
}

static bool route_has_open_files(const struct cluster_vfs_route *route)
{
    if (route == NULL) {
        return false;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_OPEN; ++i)
    {
        if (g_open_files[i].used && g_open_files[i].route == route)
        {
            return true;
        }
    }

    return false;
}

static void invalidate_open_files_for_route(struct cluster_vfs_route *route)
{
    if (route == NULL) {
        return;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_OPEN; ++i)
    {
        if (g_open_files[i].used && g_open_files[i].route == route)
        {
            reset_open_file(&g_open_files[i]);
        }
    }
}

static int allocate_generated_target_name(char *out_name, size_t out_cap)
{
    unsigned candidate;

    if (out_name == NULL || out_cap == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    for (candidate = 1u; candidate < 1000u; ++candidate)
    {
        bool used = false;
        int written;

        written = snprintf(out_name, out_cap, "mcu%u", candidate);
        if (written <= 0 || (size_t)written >= out_cap)
        {
            return -(int)M9P_ERR_EMSIZE;
        }

        for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
        {
            if (g_routes[i].m9p_state != CLUSTER_VFS_M9P_EMPTY &&
                strcmp(g_routes[i].target, out_name) == 0)
            {
                used = true;
                break;
            }
        }

        if (!used)
        {
            return 0;
        }
    }

    return -(int)M9P_ERR_EBUSY;
}

static bool mode_allows_read(uint8_t mode)
{
    uint8_t access = mode & 0x03u;
    return access == M9P_OREAD || access == M9P_ORDWR;
}

static bool mode_allows_write(uint8_t mode)
{
    uint8_t access = mode & 0x03u;
    return access == M9P_OWRITE || access == M9P_ORDWR;
}

static int resolve_path(const char *path,
                        struct cluster_vfs_route **out_route,
                        char *remote_path,
                        size_t remote_cap)
{
    if (!path || !out_route || !remote_path || remote_cap == 0)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    if (path[0] != '/' || path[1] == '\0')
    {
        return -(int)M9P_ERR_EINVAL;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].m9p_state != CLUSTER_VFS_M9P_ATTACHED || !route_is_reachable(&g_routes[i]))
        {
            continue;
        }

        const size_t target_len = strlen(g_routes[i].target);
        const char *path_target = path + 1;

        if (strncmp(path_target, g_routes[i].target, target_len) != 0)
        {
            continue;
        }

        if (path_target[target_len] != '\0' && path_target[target_len] != '/')
        {
            continue;
        }

        const char *mapped_path = path_target + target_len;// 路径映射：去掉 "/mcuN" 前缀
        if (mapped_path[0] == '\0')
        {
            mapped_path = "/";
        }

        if (strlen(mapped_path) >= remote_cap)
        {
            return -(int)M9P_ERR_EMSIZE;
        }

        strcpy(remote_path, mapped_path);
        *out_route = &g_routes[i];
        return 0;
    }

    return -(int)M9P_ERR_ENOENT;
}

int cluster_vfs_init(void)
{
    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        reset_route(&g_routes[i]);
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_OPEN; ++i)
    {
        reset_open_file(&g_open_files[i]);
    }
    return 0;
}

int cluster_vfs_bind_mesh_cluster(struct cluster *mesh_cluster)
{
    if (mesh_cluster == NULL)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    g_mesh_cluster = mesh_cluster;
    return 0;
}

int cluster_vfs_discover_node(
    uint8_t mesh_addr,
    const uint8_t hw_uid[CLUSTER_VFS_UID_LEN],
    struct m9p_client *client,
    const char **out_target,
    bool *out_reused_mapping)
{
    struct cluster_vfs_route *conflict_route;
    struct cluster_vfs_route *route;
    bool reused_mapping = false;

    if (hw_uid == NULL || client == NULL)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    /*
     * 同一个 mesh 地址在重编址/旧节点离线后，可能被新的硬件 UID 复用。
     * 因此先按“当前地址”查找活动映射，若 UID 不一致，则把旧映射回退到
     * 离线/NEW 状态，保留旧名字给旧 UID，避免把两个不同硬件误并成一个节点。
     */
    conflict_route = find_route_by_mesh_addr(mesh_addr);
    if (conflict_route != NULL &&
        (!conflict_route->has_hw_uid || memcmp(conflict_route->hw_uid, hw_uid, CLUSTER_VFS_UID_LEN) != 0))
    {
        invalidate_open_files_for_route(conflict_route);
        reset_client_session(conflict_route->client);
        conflict_route->mesh_addr = CLUSTER_VFS_UNASSIGNED_ADDR;
        conflict_route->m9p_state = CLUSTER_VFS_M9P_NEW;
    }

    /*
     * 发现流程以 UID 为主键，而不是 mesh 地址：
     * - 找到历史 UID：复用旧名字；
     * - 找不到历史 UID：新分配 mcuN。
     */
    route = find_route_by_uid(hw_uid);
    if (route == NULL)
    {
        route = alloc_route();
        if (route == NULL)
        {
            return -(int)M9P_ERR_EBUSY;
        }

        reset_route(route);
        if (allocate_generated_target_name(route->target, sizeof(route->target)) < 0)
        {
            reset_route(route);
            return -(int)M9P_ERR_EBUSY;
        }
    }
    else
    {
        reused_mapping = true;
        invalidate_open_files_for_route(route);
        if (route->client != NULL && route->client != client)
        {
            reset_client_session(route->client);
        }
    }

    reset_client_session(client);
    route->client = client;
    route->mesh_addr = mesh_addr;
    memcpy(route->hw_uid, hw_uid, CLUSTER_VFS_UID_LEN);
    route->has_hw_uid = true;
    route->m9p_state = CLUSTER_VFS_M9P_NEW;

    if (out_target != NULL)
    {
        *out_target = route->target;
    }
    if (out_reused_mapping != NULL)
    {
        *out_reused_mapping = reused_mapping;
    }

    return 0;
}

static int cluster_vfs_mark_node_offline(uint8_t mesh_addr)
{
    struct cluster_vfs_route *route;

    route = find_route_by_mesh_addr(mesh_addr);
    if (route == NULL)
    {
        return -(int)M9P_ERR_ENOENT;
    }

    /*
     * 节点已经离线时，不能再信任任何本地 fd <-> 远端 fid 映射。
     * 这里直接作废本地打开表项，并把 9P 会话状态回退到 NEW。
     * 名字和 UID 映射会保留，用于后续同一硬件重连复用名字。
     */
    invalidate_open_files_for_route(route);
    reset_client_session(route->client);
    route->mesh_addr = CLUSTER_VFS_UNASSIGNED_ADDR;
    route->m9p_state = CLUSTER_VFS_M9P_NEW;
    return 0;
}

int cluster_vfs_refresh_node_from_cluster(uint8_t mesh_addr, bool *out_reachable)
{
    int rc;

    if (out_reachable == NULL)
    {
        return -(int)M9P_ERR_EINVAL;
    }
    if (g_mesh_cluster == NULL)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    rc = cluster_can_reach(g_mesh_cluster, mesh_addr, out_reachable);
    if (rc != 0)
    {
        return rc;
    }

    if (!*out_reachable)
    {
        rc = cluster_vfs_mark_node_offline(mesh_addr);
        if (rc == -(int)M9P_ERR_ENOENT)
        {
            return 0;
        }
        return rc;
    }

    return 0;
}

int cluster_vfs_refresh_all_nodes_from_cluster(size_t *out_offline_count)
{
    size_t offline_count = 0u;
    size_t i;

    if (g_mesh_cluster == NULL)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    for (i = 0u; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        bool reachable = false;
        bool was_online;
        int rc;

        if (g_routes[i].m9p_state == CLUSTER_VFS_M9P_EMPTY)
        {
            continue;
        }
        if (g_routes[i].mesh_addr == CLUSTER_VFS_UNASSIGNED_ADDR)
        {
            continue;
        }

        was_online = route_is_reachable(&g_routes[i]);
        rc = cluster_can_reach(g_mesh_cluster, g_routes[i].mesh_addr, &reachable);
        if (rc != 0)
        {
            return rc;
        }

        if (!reachable)
        {
            rc = cluster_vfs_mark_node_offline(g_routes[i].mesh_addr);
            if (rc != 0 && rc != -(int)M9P_ERR_ENOENT)
            {
                return rc;
            }
            if (was_online)
            {
                ++offline_count;
            }
        }
    }

    if (out_offline_count != NULL)
    {
        *out_offline_count = offline_count;
    }

    return 0;
}

int cluster_vfs_attach(const char *target)
{
    struct cluster_vfs_route *route;

    if (!target)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    route = find_route(target);
    if (route == NULL)
    {
        return -(int)M9P_ERR_ENOENT;
    }
    if (route->client == NULL)
    {
        return -(int)M9P_ERR_ENOTSUP;
    }
    if (!route_is_reachable(route))
    {
        return -(int)M9P_ERR_EAGAIN;
    }
    if (route->m9p_state == CLUSTER_VFS_M9P_ATTACHED)
    {
        return 0;
    }

    reset_client_session(route->client);
    {
        int ret = m9p_client_attach(route->client, M9P_DEFAULT_MSIZE, M9P_DEFAULT_INFLIGHT, 0);

        if (ret < 0)
        {
            return ret;
        }
    }
    route->m9p_state = CLUSTER_VFS_M9P_ATTACHED;
    return 0;
}

int cluster_vfs_detach(const char *target)
{
    struct cluster_vfs_route *route;

    if (!target)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    route = find_route(target);
    if (route == NULL)
    {
        return -(int)M9P_ERR_ENOENT;
    }

    if (route_has_open_files(route))
    {
        return -(int)M9P_ERR_EBUSY;
    }

    reset_client_session(route->client);
    route->m9p_state = CLUSTER_VFS_M9P_NEW;
    return 0;
}

int cluster_vfs_open(const char *path, uint8_t mode, uint16_t *out_fd)
{
    if (!path || !out_fd)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    struct cluster_vfs_route *route;
    char remote_path[M9P_MAX_PATH_LEN + 1u];
    int ret = resolve_path(path, &route, remote_path, sizeof(remote_path));
    if (ret < 0)
    {
        return ret;
    }

    struct cluster_vfs_file *file = alloc_open_file();
    if (!file)
    {
        return -(int)M9P_ERR_EBUSY;
    }

    struct m9p_open_result result;
    ret = m9p_client_open_path(route->client, remote_path, mode, &file->remote_fid, &result);
    if (ret < 0)
    {
        return ret;
    }
    file->used = true;
    file->route = route;
    file->qid = result.qid;
    file->mode = mode;
    file->offset = 0;
    *out_fd = (uint16_t)(file - g_open_files);
    return 0;
}

int cluster_vfs_read(uint16_t fd,
                     uint8_t *buf,
                     uint16_t *in_out_len)
{
    if (fd >= CLUSTER_VFS_MAX_OPEN || !buf || !in_out_len)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    struct cluster_vfs_file *file = &g_open_files[fd];
    if (!file->used || !mode_allows_read(file->mode))
    {
        return -(int)M9P_ERR_EFID;
    }

    int ret = m9p_client_read(file->route->client, file->remote_fid, file->offset, buf, in_out_len);
    if (ret < 0)
    {
        return ret;
    }

    file->offset += *in_out_len; // 推进本地 offset

    return 0;
}

int cluster_vfs_write(uint16_t fd,
                      const uint8_t *data,
                      uint16_t len,
                      uint16_t *out_written)
{

    if (fd >= CLUSTER_VFS_MAX_OPEN || !data || !out_written)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    struct cluster_vfs_file *file = &g_open_files[fd];
    if (!file->used || !mode_allows_write(file->mode))
    {
        return -(int)M9P_ERR_EFID;
    }

    int ret = m9p_client_write(file->route->client, file->remote_fid, file->offset, data, len, out_written);
    if (ret < 0)
    {
        return ret;
    }

    file->offset += *out_written; // 推进本地 offset

    return 0;
}

int cluster_vfs_read_path(const char *path,
                          uint8_t *buf,
                          uint16_t *in_out_len)
{
    uint16_t fd;
    int ret;
    int close_ret;

    ret = cluster_vfs_open(path, M9P_OREAD, &fd);
    if (ret < 0)
    {
        return ret;
    }

    if ((g_open_files[fd].qid.type & M9P_QID_DIR) != 0u)
    {
        (void)cluster_vfs_close(fd);
        return -(int)M9P_ERR_EISDIR;
    }

    ret = cluster_vfs_read(fd, buf, in_out_len);
    close_ret = cluster_vfs_close(fd);
    if (ret < 0)
    {
        return ret;
    }
    return close_ret;
}

int cluster_vfs_write_path(const char *path,
                           const uint8_t *data,
                           uint16_t len,
                           uint16_t *out_written)
{
    uint16_t fd;
    int ret;
    int close_ret;

    ret = cluster_vfs_open(path, M9P_OWRITE, &fd);
    if (ret < 0)
    {
        return ret;
    }

    if ((g_open_files[fd].qid.type & M9P_QID_DIR) != 0u)
    {
        (void)cluster_vfs_close(fd);
        return -(int)M9P_ERR_EISDIR;
    }

    ret = cluster_vfs_write(fd, data, len, out_written);
    close_ret = cluster_vfs_close(fd);
    if (ret < 0)
    {
        return ret;
    }
    return close_ret;
}

static int list_root(struct m9p_dirent *entries,
                     size_t max_entries,
                     size_t *out_count)
{
    size_t produced = 0u;

    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES && produced < max_entries; ++i)
    {
        if (g_routes[i].m9p_state == CLUSTER_VFS_M9P_EMPTY)
        {
            continue;
        }

        memset(&entries[produced], 0, sizeof(entries[produced]));
        entries[produced].qid.type = M9P_QID_DIR | M9P_QID_VIRTUAL;
        entries[produced].qid.object_id = (uint32_t)(i + 1u);
        entries[produced].flags = M9P_STAT_DIR | M9P_STAT_VIRTUAL;
        strncpy(entries[produced].name, g_routes[i].target, M9P_MAX_NAME_LEN);
        entries[produced].name[M9P_MAX_NAME_LEN] = '\0';
        ++produced;
    }

    *out_count = produced;
    return 0;
}

int cluster_vfs_list(const char *path,
                     struct m9p_dirent *entries,
                     size_t max_entries,
                     size_t *out_count)
{
    uint16_t fd;
    struct cluster_vfs_file *file;
    uint32_t dir_offset = 0u;
    size_t produced = 0u;
    int ret;
    int close_ret;

    if (!path || !out_count || (max_entries > 0u && !entries))
    {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    if (max_entries == 0u)
    {
        return 0;
    }

    if (strcmp(path, "/") == 0)
    {
        return list_root(entries, max_entries, out_count);
    }

    ret = cluster_vfs_open(path, M9P_OREAD, &fd);
    if (ret < 0)
    {
        return ret;
    }

    file = &g_open_files[fd];
    if ((file->qid.type & M9P_QID_DIR) == 0u)
    {
        (void)cluster_vfs_close(fd);
        return -(int)M9P_ERR_ENOTDIR;
    }

    while (produced < max_entries)
    {
        uint8_t read_buf[M9P_CLIENT_BUFFER_CAP];
        uint16_t read_len = sizeof(read_buf);
        size_t parsed;

        ret = m9p_client_read(file->route->client, file->remote_fid, dir_offset, read_buf, &read_len);
        if (ret < 0)
        {
            break;
        }

        if (read_len == 0u)
        {
            break;
        }

        parsed = m9p_parse_dirents(read_buf,
                                   read_len,
                                   entries + produced,
                                   max_entries - produced);
        if (parsed == 0u)
        {
            ret = -(int)M9P_ERR_EIO;
            break;
        }

        produced += parsed;
        dir_offset += (uint32_t)parsed;
    }

    *out_count = produced;
    close_ret = cluster_vfs_close(fd);
    if (ret < 0)
    {
        return ret;
    }
    return close_ret;
}

int cluster_vfs_stat(const char *path,
                     struct m9p_stat *out_stat)
{

    if (!path || !out_stat)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    if (strcmp(path, "/") == 0)
    {
        memset(out_stat, 0, sizeof(*out_stat));
        out_stat->qid.type = M9P_QID_DIR | M9P_QID_VIRTUAL;
        out_stat->flags = M9P_STAT_DIR | M9P_STAT_VIRTUAL;
        strcpy(out_stat->name, "/");
        return 0;
    }

    struct cluster_vfs_route *route;
    char remote_path[M9P_MAX_PATH_LEN + 1u];
    int ret = resolve_path(path, &route, remote_path, sizeof(remote_path));
    if (ret < 0)
    {
        return ret;
    }

    struct m9p_client *client = route->client;
    uint16_t fid;
    struct m9p_qid qid;
    ret = m9p_client_walk_path(client, remote_path, &fid, &qid);
    if (ret < 0)
    {
        return ret;
    }
    ret = m9p_client_stat(client, fid, out_stat);
    if (ret < 0)
    {
        (void)m9p_client_clunk(client, fid);
        return ret;
    }
    return m9p_client_clunk(client, fid); // 释放临时 fid
}

int cluster_vfs_close(uint16_t fd){
    int rc;

    if (fd >= CLUSTER_VFS_MAX_OPEN)
    {
        return -(int)M9P_ERR_EINVAL;
    }
    struct cluster_vfs_file *file = &g_open_files[fd];
    if (!file->used)
    {
        return -(int)M9P_ERR_EFID;
    }

    rc = m9p_client_clunk(file->route->client, file->remote_fid); // 释放远端 fid
    reset_open_file(file);
    return rc;
}
