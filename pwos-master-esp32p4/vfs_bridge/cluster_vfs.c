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

    for (size_t i = 0; i < CLUSTER_VFS_MAX_ROUTES; ++i)
    {
        if (g_routes[i].state == CLUSTER_VFS_ROUTE_ATTACHED &&
            strncmp(path, g_routes[i].target, strlen(g_routes[i].target)) == 0)
        {
            struct cluster_vfs_file *file = alloc_open_file();
            if (!file)
            {
                return -(int)M9P_ERR_EBUSY;
            }
            file->used = true;
            file->local_fd = (uint16_t)(file - g_open_files); // 计算 local_fd，即 open_files 数组中的索引
            file->route = &g_routes[i];
            m9p_client_walk_path(g_routes[i].client, path, &file->remote_fid, &file->qid);
            file->mode = mode;
            file->offset = 0;
            *out_fd = file->local_fd; // 返回文件描述符，即 open_files 数组中的索引
            return 0;
        }
    }
    return -(int)M9P_ERR_ENOENT;
}