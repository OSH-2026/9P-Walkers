#include "cluster_vfs.h"
#include <string.h>

static struct cluster_vfs_route g_routes[CLUSTER_VFS_MAX_ROUTES];
static struct cluster_vfs_file g_open_files[CLUSTER_VFS_MAX_OPEN];

static struct cluster_vfs_route *alloc_route(void)
{
    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].state == CLUSTER_VFS_ROUTE_EMPTY)
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
        if (g_routes[i].state != CLUSTER_VFS_ROUTE_ATTACHED)
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

        const char *mapped_path = path;
        if (strcmp(g_routes[i].target, g_routes[i].next_hop) == 0)
        {
            mapped_path = path_target + target_len;
            if (mapped_path[0] == '\0')
            {
                mapped_path = "/";
            }
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
        g_routes[i].state = CLUSTER_VFS_ROUTE_EMPTY;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_OPEN; ++i)
    {
        g_open_files[i].used = false;
    }
    return 0;
}

// 暂时只支持直接添加单跳路由的方式
int cluster_vfs_add_direct(const char *target,
                           struct m9p_client *client)
{
    if (!target || !client)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    struct cluster_vfs_route *route = alloc_route();
    if (!route)
    {
        return -(int)M9P_ERR_EBUSY;
    }

    strncpy(route->target, target, CLUSTER_VFS_MAX_NAME);
    route->target[CLUSTER_VFS_MAX_NAME - 1] = '\0';
    strncpy(route->next_hop, target, CLUSTER_VFS_MAX_NAME);
    route->next_hop[CLUSTER_VFS_MAX_NAME - 1] = '\0';
    route->client = client;
    route->state = CLUSTER_VFS_ROUTE_READY;

    return 0;
}

int cluster_vfs_remove_route(const char *target)
{
    if (!target)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].state != CLUSTER_VFS_ROUTE_EMPTY &&
            strcmp(g_routes[i].target, target) == 0)
        {

            for (size_t j = 0; j < CLUSTER_VFS_MAX_OPEN; ++j)
            {
                if (g_open_files[j].used &&
                    strcmp(g_open_files[j].route->target, target) == 0)
                {
                    return -(int)M9P_ERR_EBUSY;
                }
            }

            g_routes[i].state = CLUSTER_VFS_ROUTE_EMPTY;
            return 0;
        }
    }
    return -(int)M9P_ERR_ENOENT;
}

int cluster_vfs_attach(const char *target)
{
    if (!target)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].state == CLUSTER_VFS_ROUTE_READY &&
            strcmp(g_routes[i].target, target) == 0)
        {
            int ret = m9p_client_attach(g_routes[i].client, M9P_DEFAULT_MSIZE, M9P_DEFAULT_INFLIGHT, 0);
            if (ret < 0)
            {
                return ret;
            }
            g_routes[i].state = CLUSTER_VFS_ROUTE_ATTACHED;
            return 0;
        }
    }
    return -(int)M9P_ERR_ENOENT;
}

int cluster_vfs_detach(const char *target)
{
    if (!target)
    {
        return -(int)M9P_ERR_EINVAL;
    }

    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].state == CLUSTER_VFS_ROUTE_ATTACHED &&
            strcmp(g_routes[i].target, target) == 0)
        {
            g_routes[i].state = CLUSTER_VFS_ROUTE_READY;
            return 0;
        }
    }
    return -(int)M9P_ERR_ENOENT;
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
    file->local_fd = (uint16_t)(file - g_open_files); // 计算 local_fd，即 open_files 数组中的索引
    file->route = route;
    file->mode = mode;
    file->offset = 0;
    *out_fd = file->local_fd; // 返回文件描述符，即 open_files 数组中的索引
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
    if (!file->used || !(file->mode & M9P_OREAD))
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
    if (!file->used || !(file->mode & M9P_OWRITE))
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

int cluster_vfs_stat(const char *path,
                     struct m9p_stat *out_stat)
{

    if (!path || !out_stat)
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
        return ret;
    }
    m9p_client_clunk(client, fid); // 释放临时 fid
    return 0;
}

int cluster_vfs_close(uint16_t fd){
    if (fd >= CLUSTER_VFS_MAX_OPEN)
    {
        return -(int)M9P_ERR_EINVAL;
    }
    struct cluster_vfs_file *file = &g_open_files[fd];
    if (!file->used)
    {
        return -(int)M9P_ERR_EFID;
    }
    file->used = false;
    m9p_client_clunk(file->route->client, file->remote_fid); // 释放远端 fid
    return 0;
}
