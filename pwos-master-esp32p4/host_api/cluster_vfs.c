#include "cluster_vfs.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    size_t index;
    uint8_t addr;
    uint32_t boot_id;
    uint32_t generation;
} pwos_cluster_vfs_route_ref_t;

static uint8_t route_is_online(const pwos_cluster_vfs_route_t *route);

static void vfs_lock(pwos_cluster_vfs_t *vfs)
{
    if (vfs != NULL && vfs->config.lock != NULL) {
        vfs->config.lock(vfs->config.lock_ctx);
    }
}

static void vfs_unlock(pwos_cluster_vfs_t *vfs)
{
    if (vfs != NULL && vfs->config.unlock != NULL) {
        vfs->config.unlock(vfs->config.lock_ctx);
    }
}

static uint8_t uid_equal(const uint32_t a[3], const uint32_t b[3])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static void set_error(pwos_cluster_vfs_t *vfs, int rc)
{
    if (vfs != NULL && rc != 0) {
        vfs_lock(vfs);
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
    }
}

static void reset_file(pwos_cluster_vfs_file_t *file)
{
    if (file == NULL) {
        return;
    }
    memset(file, 0, sizeof(*file));
}

static void reset_route(pwos_cluster_vfs_route_t *route)
{
    if (route == NULL) {
        return;
    }
    memset(route, 0, sizeof(*route));
    route->state = PWOS_CLUSTER_VFS_ROUTE_EMPTY;
    route->addr = PWOS_CLUSTER_VFS_UNASSIGNED_ADDR;
}

static uint32_t next_generation(uint32_t *value)
{
    ++(*value);
    if (*value == 0u) {
        ++(*value);
    }
    return *value;
}

static void route_to_ref(
    const pwos_cluster_vfs_t *vfs,
    const pwos_cluster_vfs_route_t *route,
    pwos_cluster_vfs_route_ref_t *out_ref)
{
    out_ref->index = (size_t)(route - vfs->routes);
    out_ref->addr = route->addr;
    out_ref->boot_id = route->boot_id;
    out_ref->generation = route->generation;
}

static uint8_t route_ref_matches_locked(
    const pwos_cluster_vfs_t *vfs,
    const pwos_cluster_vfs_route_ref_t *ref)
{
    const pwos_cluster_vfs_route_t *route;

    if (ref->index >= PWOS_CLUSTER_VFS_MAX_ROUTES) {
        return 0u;
    }
    route = &vfs->routes[ref->index];
    return route_is_online(route) != 0u &&
        route->addr == ref->addr &&
        route->boot_id == ref->boot_id &&
        route->generation == ref->generation;
}

static uint8_t route_is_online(const pwos_cluster_vfs_route_t *route)
{
    if (route == NULL) {
        return 0u;
    }
    if (route->state != PWOS_CLUSTER_VFS_ROUTE_NEW &&
        route->state != PWOS_CLUSTER_VFS_ROUTE_ATTACHED) {
        return 0u;
    }
    return route->addr != PWOS_CLUSTER_VFS_UNASSIGNED_ADDR ? 1u : 0u;
}

static uint8_t mode_allows_read(uint8_t mode)
{
    uint8_t access = (uint8_t)(mode & 0x03u);
    return access == M9P_OREAD || access == M9P_ORDWR;
}

static uint8_t mode_allows_write(uint8_t mode)
{
    uint8_t access = (uint8_t)(mode & 0x03u);
    return access == M9P_OWRITE || access == M9P_ORDWR;
}

static pwos_cluster_vfs_route_t *find_route_mut(
    pwos_cluster_vfs_t *vfs,
    const char *target)
{
    size_t i;

    if (vfs == NULL || target == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        if (vfs->routes[i].state != PWOS_CLUSTER_VFS_ROUTE_EMPTY &&
            strcmp(vfs->routes[i].target, target) == 0) {
            return &vfs->routes[i];
        }
    }
    return NULL;
}

static pwos_cluster_vfs_route_t *find_route_by_uid_mut(
    pwos_cluster_vfs_t *vfs,
    const uint32_t uid[3])
{
    size_t i;

    if (vfs == NULL || uid == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        if (vfs->routes[i].state != PWOS_CLUSTER_VFS_ROUTE_EMPTY &&
            uid_equal(vfs->routes[i].uid, uid)) {
            return &vfs->routes[i];
        }
    }
    return NULL;
}

static pwos_cluster_vfs_route_t *find_route_by_addr_mut(
    pwos_cluster_vfs_t *vfs,
    uint8_t addr)
{
    size_t i;

    if (vfs == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        if (vfs->routes[i].state != PWOS_CLUSTER_VFS_ROUTE_EMPTY &&
            vfs->routes[i].addr == addr) {
            return &vfs->routes[i];
        }
    }
    return NULL;
}

static pwos_cluster_vfs_route_t *alloc_route(pwos_cluster_vfs_t *vfs)
{
    size_t i;

    if (vfs == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        if (vfs->routes[i].state == PWOS_CLUSTER_VFS_ROUTE_EMPTY) {
            return &vfs->routes[i];
        }
    }
    return NULL;
}

static pwos_cluster_vfs_file_t *alloc_file(pwos_cluster_vfs_t *vfs)
{
    size_t i;

    if (vfs == NULL) {
        return NULL;
    }

    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_OPEN; ++i) {
        if (vfs->files[i].used == 0u) {
            return &vfs->files[i];
        }
    }
    return NULL;
}

static void invalidate_files_for_route(pwos_cluster_vfs_t *vfs, size_t route_index)
{
    size_t i;

    if (vfs == NULL || route_index >= PWOS_CLUSTER_VFS_MAX_ROUTES) {
        return;
    }

    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_OPEN; ++i) {
        if (vfs->files[i].used != 0u && vfs->files[i].route_index == route_index) {
            reset_file(&vfs->files[i]);
        }
    }
}

static int allocate_target_name(
    const pwos_cluster_vfs_t *vfs,
    char *out_name,
    size_t out_cap)
{
    unsigned candidate;

    if (vfs == NULL || out_name == NULL || out_cap == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }

    for (candidate = 1u; candidate < 1000u; ++candidate) {
        char name[PWOS_CLUSTER_VFS_MAX_NAME];
        size_t i;
        int written;
        uint8_t used = 0u;

        written = snprintf(name, sizeof(name), "mcu%u", candidate);
        if (written <= 0 || (size_t)written >= sizeof(name) ||
            (size_t)written >= out_cap) {
            return -(int)M9P_ERR_EMSIZE;
        }

        for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
            if (vfs->routes[i].state != PWOS_CLUSTER_VFS_ROUTE_EMPTY &&
                strcmp(vfs->routes[i].target, name) == 0) {
                used = 1u;
                break;
            }
        }
        if (used == 0u) {
            memcpy(out_name, name, (size_t)written + 1u);
            return 0;
        }
    }
    return -(int)M9P_ERR_EBUSY;
}

static size_t route_index_of(
    const pwos_cluster_vfs_t *vfs,
    const pwos_cluster_vfs_route_t *route)
{
    if (vfs == NULL || route == NULL) {
        return PWOS_CLUSTER_VFS_MAX_ROUTES;
    }
    return (size_t)(route - vfs->routes);
}

static uint8_t coordinator_has_uid(
    const pwos_host_coordinator_t *coordinator,
    const uint32_t uid[3])
{
    size_t i;

    if (coordinator == NULL || uid == NULL) {
        return 0u;
    }

    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        if (coordinator->nodes[i].valid != 0u &&
            uid_equal(coordinator->nodes[i].uid, uid)) {
            return 1u;
        }
    }
    return 0u;
}

static int sync_one_node(
    pwos_cluster_vfs_t *vfs,
    const pwos_host_node_entry_t *node)
{
    pwos_cluster_vfs_route_t *conflict;
    pwos_cluster_vfs_route_t *route;
    size_t route_index;
    uint8_t mapping_changed = 0u;

    if (vfs == NULL || node == NULL || vfs->sessions == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    /*
     * mesh 地址是租约结果，不是硬件身份。若一个地址被不同 UID 复用，
     * 旧 route 必须先下线，避免 fd 指向错误节点。
     */
    conflict = find_route_by_addr_mut(vfs, node->addr);
    if (conflict != NULL && !uid_equal(conflict->uid, node->uid)) {
        route_index = route_index_of(vfs, conflict);
        invalidate_files_for_route(vfs, route_index);
        pwos_session_manager_reset_node(vfs->sessions, conflict->addr);
        conflict->addr = PWOS_CLUSTER_VFS_UNASSIGNED_ADDR;
        conflict->state = PWOS_CLUSTER_VFS_ROUTE_OFFLINE;
        conflict->generation = next_generation(&vfs->next_route_generation);
        ++vfs->stats.detached;
    }

    route = find_route_by_uid_mut(vfs, node->uid);
    if (route == NULL) {
        route = alloc_route(vfs);
        if (route == NULL) {
            return -(int)M9P_ERR_EBUSY;
        }
        reset_route(route);
        if (allocate_target_name(vfs, route->target, sizeof(route->target)) != 0) {
            reset_route(route);
            return -(int)M9P_ERR_EBUSY;
        }
        route->state = PWOS_CLUSTER_VFS_ROUTE_NEW;
        route->addr = node->addr;
        route->uid[0] = node->uid[0];
        route->uid[1] = node->uid[1];
        route->uid[2] = node->uid[2];
        route->boot_id = node->boot_id;
        route->generation = next_generation(&vfs->next_route_generation);
        mapping_changed = 1u;
        ++vfs->stats.discovered;
    } else {
        mapping_changed =
            route->addr != node->addr ||
            route->boot_id != node->boot_id ||
            route->state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE;
        if (mapping_changed != 0u) {
            route_index = route_index_of(vfs, route);
            invalidate_files_for_route(vfs, route_index);
            if (route->addr != node->addr &&
                route->addr != PWOS_CLUSTER_VFS_UNASSIGNED_ADDR) {
                pwos_session_manager_reset_node(vfs->sessions, route->addr);
            }
            ++vfs->stats.rediscovered;
            route->generation = next_generation(&vfs->next_route_generation);
        }
        route->addr = node->addr;
        route->boot_id = node->boot_id;
        route->uid[0] = node->uid[0];
        route->uid[1] = node->uid[1];
        route->uid[2] = node->uid[2];
        if (mapping_changed != 0u || route->state == PWOS_CLUSTER_VFS_ROUTE_EMPTY) {
            route->state = PWOS_CLUSTER_VFS_ROUTE_NEW;
        }
    }

    return pwos_session_manager_update_node(vfs->sessions, route->addr, route->boot_id);
}

static int ensure_attached(
    pwos_cluster_vfs_t *vfs,
    const pwos_cluster_vfs_route_ref_t *ref,
    uint32_t deadline_ms)
{
    int rc;

    if (vfs == NULL || ref == NULL || vfs->sessions == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    vfs_lock(vfs);
    if (route_ref_matches_locked(vfs, ref) == 0u) {
        vfs_unlock(vfs);
        return PWOS_SESSION_ERR_NO_ROUTE;
    }
    if (vfs->routes[ref->index].state == PWOS_CLUSTER_VFS_ROUTE_ATTACHED) {
        vfs_unlock(vfs);
        return 0;
    }
    vfs_unlock(vfs);

    rc = pwos_session_manager_attach(
        vfs->sessions,
        ref->addr,
        ref->boot_id,
        deadline_ms);

    vfs_lock(vfs);
    if (rc == 0) {
        if (route_ref_matches_locked(vfs, ref) != 0u) {
            vfs->routes[ref->index].state = PWOS_CLUSTER_VFS_ROUTE_ATTACHED;
            ++vfs->stats.attach_ok;
        } else {
            rc = PWOS_SESSION_ERR_STALE_BOOT;
            ++vfs->stats.attach_fail;
            vfs->stats.last_error = (int32_t)rc;
        }
    } else {
        ++vfs->stats.attach_fail;
        vfs->stats.last_error = (int32_t)rc;
    }
    vfs_unlock(vfs);
    return rc;
}

static int resolve_path(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    uint32_t deadline_ms,
    pwos_cluster_vfs_route_ref_t *out_ref,
    char *remote_path,
    size_t remote_cap)
{
    size_t i;
    uint8_t matched_offline = 0u;

    if (vfs == NULL || path == NULL || out_ref == NULL ||
        remote_path == NULL || remote_cap == 0u) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (path[0] != '/' || path[1] == '\0') {
        return -(int)M9P_ERR_EINVAL;
    }

    vfs_lock(vfs);
    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        const char *path_target;
        const char *mapped_path;
        size_t target_len;
        int rc;

        if (vfs->routes[i].state == PWOS_CLUSTER_VFS_ROUTE_EMPTY) {
            continue;
        }

        target_len = strlen(vfs->routes[i].target);
        path_target = path + 1u;
        if (strncmp(path_target, vfs->routes[i].target, target_len) != 0) {
            continue;
        }
        if (path_target[target_len] != '\0' && path_target[target_len] != '/') {
            continue;
        }
        if (route_is_online(&vfs->routes[i]) == 0u) {
            matched_offline = 1u;
            continue;
        }

        mapped_path = path_target + target_len;
        if (mapped_path[0] == '\0') {
            mapped_path = "/";
        }
        if (strlen(mapped_path) >= remote_cap) {
            vfs_unlock(vfs);
            return -(int)M9P_ERR_EMSIZE;
        }

        memcpy(remote_path, mapped_path, strlen(mapped_path) + 1u);
        route_to_ref(vfs, &vfs->routes[i], out_ref);
        vfs_unlock(vfs);
        rc = ensure_attached(vfs, out_ref, deadline_ms);
        return rc;
    }

    vfs_unlock(vfs);
    return matched_offline != 0u ? PWOS_SESSION_ERR_NO_ROUTE : -(int)M9P_ERR_ENOENT;
}

int pwos_cluster_vfs_init(
    pwos_cluster_vfs_t *vfs,
    pwos_session_manager_t *sessions)
{
    return pwos_cluster_vfs_init_with_config(vfs, sessions, NULL);
}

int pwos_cluster_vfs_init_with_config(
    pwos_cluster_vfs_t *vfs,
    pwos_session_manager_t *sessions,
    const pwos_cluster_vfs_config_t *config)
{
    size_t i;

    if (vfs == NULL || sessions == NULL ||
        (config != NULL && ((config->lock == NULL) != (config->unlock == NULL)))) {
        return -(int)M9P_ERR_EINVAL;
    }

    memset(vfs, 0, sizeof(*vfs));
    vfs->sessions = sessions;
    if (config != NULL) {
        vfs->config = *config;
    }
    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        reset_route(&vfs->routes[i]);
    }
    return 0;
}

int pwos_cluster_vfs_sync_from_coordinator(
    pwos_cluster_vfs_t *vfs,
    const pwos_host_coordinator_t *coordinator)
{
    size_t i;

    if (vfs == NULL || coordinator == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    vfs_lock(vfs);
    ++vfs->stats.sync_count;
    for (i = 0u; i < PWOS_HOST_COORDINATOR_MAX_NODES; ++i) {
        int rc;

        if (coordinator->nodes[i].valid == 0u) {
            continue;
        }
        rc = sync_one_node(vfs, &coordinator->nodes[i]);
        if (rc != 0) {
            vfs->stats.last_error = (int32_t)rc;
            vfs_unlock(vfs);
            return rc;
        }
    }

    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES; ++i) {
        if (vfs->routes[i].state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ||
            vfs->routes[i].state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE) {
            continue;
        }
        if (coordinator_has_uid(coordinator, vfs->routes[i].uid) == 0u) {
            invalidate_files_for_route(vfs, i);
            if (vfs->routes[i].addr != PWOS_CLUSTER_VFS_UNASSIGNED_ADDR) {
                pwos_session_manager_reset_node(vfs->sessions, vfs->routes[i].addr);
            }
            vfs->routes[i].addr = PWOS_CLUSTER_VFS_UNASSIGNED_ADDR;
            vfs->routes[i].state = PWOS_CLUSTER_VFS_ROUTE_OFFLINE;
            vfs->routes[i].generation = next_generation(&vfs->next_route_generation);
            ++vfs->stats.detached;
        }
    }

    vfs_unlock(vfs);
    return 0;
}

int pwos_cluster_vfs_get_route(
    pwos_cluster_vfs_t *vfs,
    size_t index,
    pwos_cluster_vfs_route_t *out_route)
{
    if (vfs == NULL || out_route == NULL || index >= PWOS_CLUSTER_VFS_MAX_ROUTES) {
        return -(int)M9P_ERR_EINVAL;
    }
    vfs_lock(vfs);
    *out_route = vfs->routes[index];
    vfs_unlock(vfs);
    return out_route->state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ?
        -(int)M9P_ERR_ENOENT : 0;
}

int pwos_cluster_vfs_attach(
    pwos_cluster_vfs_t *vfs,
    const char *target,
    uint32_t deadline_ms)
{
    pwos_cluster_vfs_route_t *route;
    pwos_cluster_vfs_route_ref_t ref;
    int rc;

    if (vfs == NULL || target == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    vfs_lock(vfs);
    route = find_route_mut(vfs, target);
    if (route == NULL) {
        vfs_unlock(vfs);
        return -(int)M9P_ERR_ENOENT;
    }
    route_to_ref(vfs, route, &ref);
    vfs_unlock(vfs);

    rc = ensure_attached(vfs, &ref, deadline_ms);
    set_error(vfs, rc);
    return rc;
}

int pwos_cluster_vfs_open(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    uint8_t mode,
    uint32_t deadline_ms,
    uint16_t *out_fd)
{
    pwos_cluster_vfs_route_ref_t route_ref;
    pwos_cluster_vfs_file_t *file;
    struct m9p_client *client;
    struct m9p_open_result result;
    char remote_path[M9P_MAX_PATH_LEN + 1u];
    uint16_t remote_fid = 0u;
    uint16_t fd;
    uint32_t file_generation;
    uint8_t session_index = 0xFFu;
    int rc;

    if (vfs == NULL || path == NULL || out_fd == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_fd = 0xFFFFu;
    rc = resolve_path(vfs, path, deadline_ms, &route_ref, remote_path, sizeof(remote_path));
    if (rc != 0) {
        vfs_lock(vfs);
        ++vfs->stats.open_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }

    vfs_lock(vfs);
    if (route_ref_matches_locked(vfs, &route_ref) == 0u) {
        rc = PWOS_SESSION_ERR_STALE_BOOT;
        ++vfs->stats.open_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }
    file = alloc_file(vfs);
    if (file == NULL) {
        rc = -(int)M9P_ERR_EBUSY;
        ++vfs->stats.open_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }
    reset_file(file);
    file->used = 2u; /* 2 表示正在打开，防止并发调用复用该槽位。 */
    file->busy = 1u;
    file->route_index = (uint8_t)route_ref.index;
    file->route_generation = route_ref.generation;
    file->generation = next_generation(&vfs->next_file_generation);
    file_generation = file->generation;
    fd = (uint16_t)(file - vfs->files);
    vfs_unlock(vfs);

    rc = pwos_session_manager_acquire_client(
        vfs->sessions,
        route_ref.addr,
        route_ref.boot_id,
        deadline_ms,
        &client,
        &session_index);
    if (rc == 0) {
        rc = m9p_client_open_path(client, remote_path, mode, &remote_fid, &result);
        pwos_session_manager_release_client(vfs->sessions, session_index);
    }

    vfs_lock(vfs);
    file = &vfs->files[fd];
    if (file->used != 2u || file->generation != file_generation ||
        route_ref_matches_locked(vfs, &route_ref) == 0u) {
        reset_file(file);
        rc = PWOS_SESSION_ERR_STALE_BOOT;
    }
    if (rc != 0) {
        reset_file(file);
        ++vfs->stats.open_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }

    file->used = 1u;
    file->busy = 0u;
    file->remote_fid = remote_fid;
    file->qid = result.qid;
    file->mode = mode;
    file->offset = 0u;
    *out_fd = fd;
    ++vfs->stats.open_ok;
    vfs_unlock(vfs);
    return 0;
}

int pwos_cluster_vfs_read(
    pwos_cluster_vfs_t *vfs,
    uint16_t fd,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    pwos_cluster_vfs_file_t snapshot;
    pwos_cluster_vfs_route_ref_t route_ref;
    struct m9p_client *client;
    uint8_t session_index = 0xFFu;
    int rc;

    if (vfs == NULL || fd >= PWOS_CLUSTER_VFS_MAX_OPEN ||
        buf == NULL || in_out_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    vfs_lock(vfs);
    snapshot = vfs->files[fd];
    if (snapshot.used != 1u || snapshot.busy != 0u ||
        mode_allows_read(snapshot.mode) == 0u ||
        snapshot.route_index >= PWOS_CLUSTER_VFS_MAX_ROUTES) {
        rc = -(int)M9P_ERR_EFID;
        ++vfs->stats.read_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }
    route_to_ref(vfs, &vfs->routes[snapshot.route_index], &route_ref);
    if (route_ref.generation != snapshot.route_generation ||
        route_ref_matches_locked(vfs, &route_ref) == 0u) {
        rc = PWOS_SESSION_ERR_STALE_BOOT;
        ++vfs->stats.read_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }
    vfs->files[fd].busy = 1u;
    vfs_unlock(vfs);

    rc = pwos_session_manager_acquire_client(
        vfs->sessions,
        route_ref.addr,
        route_ref.boot_id,
        deadline_ms,
        &client,
        &session_index);
    if (rc == 0) {
        rc = m9p_client_read(
            client,
            snapshot.remote_fid,
            snapshot.offset,
            buf,
            in_out_len);
        pwos_session_manager_release_client(vfs->sessions, session_index);
    }

    vfs_lock(vfs);
    if (vfs->files[fd].used != 1u ||
        vfs->files[fd].generation != snapshot.generation ||
        route_ref_matches_locked(vfs, &route_ref) == 0u) {
        rc = PWOS_SESSION_ERR_STALE_BOOT;
    } else {
        vfs->files[fd].busy = 0u;
    }
    if (rc != 0) {
        ++vfs->stats.read_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }

    vfs->files[fd].offset += *in_out_len;
    ++vfs->stats.read_ok;
    vfs_unlock(vfs);
    return 0;
}

int pwos_cluster_vfs_write(
    pwos_cluster_vfs_t *vfs,
    uint16_t fd,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    pwos_cluster_vfs_file_t snapshot;
    pwos_cluster_vfs_route_ref_t route_ref;
    struct m9p_client *client;
    uint8_t session_index = 0xFFu;
    int rc;

    if (vfs == NULL || fd >= PWOS_CLUSTER_VFS_MAX_OPEN ||
        data == NULL || out_written == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    vfs_lock(vfs);
    snapshot = vfs->files[fd];
    if (snapshot.used != 1u || snapshot.busy != 0u ||
        mode_allows_write(snapshot.mode) == 0u ||
        snapshot.route_index >= PWOS_CLUSTER_VFS_MAX_ROUTES) {
        rc = -(int)M9P_ERR_EFID;
        ++vfs->stats.write_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }
    route_to_ref(vfs, &vfs->routes[snapshot.route_index], &route_ref);
    if (route_ref.generation != snapshot.route_generation ||
        route_ref_matches_locked(vfs, &route_ref) == 0u) {
        rc = PWOS_SESSION_ERR_STALE_BOOT;
        ++vfs->stats.write_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }
    vfs->files[fd].busy = 1u;
    vfs_unlock(vfs);

    rc = pwos_session_manager_acquire_client(
        vfs->sessions,
        route_ref.addr,
        route_ref.boot_id,
        deadline_ms,
        &client,
        &session_index);
    if (rc == 0) {
        rc = m9p_client_write(
            client,
            snapshot.remote_fid,
            snapshot.offset,
            data,
            len,
            out_written);
        pwos_session_manager_release_client(vfs->sessions, session_index);
    }

    vfs_lock(vfs);
    if (vfs->files[fd].used != 1u ||
        vfs->files[fd].generation != snapshot.generation ||
        route_ref_matches_locked(vfs, &route_ref) == 0u) {
        rc = PWOS_SESSION_ERR_STALE_BOOT;
    } else {
        vfs->files[fd].busy = 0u;
    }
    if (rc != 0) {
        ++vfs->stats.write_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }

    vfs->files[fd].offset += *out_written;
    ++vfs->stats.write_ok;
    vfs_unlock(vfs);
    return 0;
}

int pwos_cluster_vfs_close(
    pwos_cluster_vfs_t *vfs,
    uint16_t fd,
    uint32_t deadline_ms)
{
    pwos_cluster_vfs_file_t snapshot;
    pwos_cluster_vfs_route_ref_t route_ref;
    struct m9p_client *client;
    uint8_t session_index = 0xFFu;
    int rc;

    if (vfs == NULL || fd >= PWOS_CLUSTER_VFS_MAX_OPEN) {
        return -(int)M9P_ERR_EINVAL;
    }

    vfs_lock(vfs);
    snapshot = vfs->files[fd];
    if (snapshot.used != 1u || snapshot.busy != 0u ||
        snapshot.route_index >= PWOS_CLUSTER_VFS_MAX_ROUTES) {
        rc = -(int)M9P_ERR_EFID;
        ++vfs->stats.close_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }
    route_to_ref(vfs, &vfs->routes[snapshot.route_index], &route_ref);
    vfs->files[fd].busy = 1u;
    vfs_unlock(vfs);

    rc = pwos_session_manager_acquire_client(
        vfs->sessions,
        route_ref.addr,
        route_ref.boot_id,
        deadline_ms,
        &client,
        &session_index);
    if (rc == 0) {
        rc = m9p_client_clunk(client, snapshot.remote_fid);
        pwos_session_manager_release_client(vfs->sessions, session_index);
    }

    vfs_lock(vfs);
    if (vfs->files[fd].generation == snapshot.generation) {
        reset_file(&vfs->files[fd]);
    } else {
        rc = PWOS_SESSION_ERR_STALE_BOOT;
    }
    if (rc != 0) {
        ++vfs->stats.close_fail;
        vfs->stats.last_error = (int32_t)rc;
        vfs_unlock(vfs);
        return rc;
    }
    ++vfs->stats.close_ok;
    vfs_unlock(vfs);
    return 0;
}

static int get_open_file_qid(
    pwos_cluster_vfs_t *vfs,
    uint16_t fd,
    struct m9p_qid *out_qid)
{
    if (vfs == NULL || out_qid == NULL || fd >= PWOS_CLUSTER_VFS_MAX_OPEN) {
        return -(int)M9P_ERR_EINVAL;
    }
    vfs_lock(vfs);
    if (vfs->files[fd].used != 1u) {
        vfs_unlock(vfs);
        return -(int)M9P_ERR_EFID;
    }
    *out_qid = vfs->files[fd].qid;
    vfs_unlock(vfs);
    return 0;
}

int pwos_cluster_vfs_read_path(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms)
{
    uint16_t fd = 0xFFFFu;
    struct m9p_qid qid;
    int rc;
    int close_rc;

    rc = pwos_cluster_vfs_open(vfs, path, M9P_OREAD, deadline_ms, &fd);
    if (rc != 0) {
        return rc;
    }

    rc = get_open_file_qid(vfs, fd, &qid);
    if (rc != 0 || (qid.type & M9P_QID_DIR) != 0u) {
        (void)pwos_cluster_vfs_close(vfs, fd, deadline_ms);
        if (rc == 0) {
            rc = -(int)M9P_ERR_EISDIR;
        }
        set_error(vfs, rc);
        return rc;
    }

    rc = pwos_cluster_vfs_read(vfs, fd, buf, in_out_len, deadline_ms);
    close_rc = pwos_cluster_vfs_close(vfs, fd, deadline_ms);
    if (rc != 0) {
        return rc;
    }
    return close_rc;
}

int pwos_cluster_vfs_write_path(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms)
{
    uint16_t fd = 0xFFFFu;
    struct m9p_qid qid;
    int rc;
    int close_rc;

    rc = pwos_cluster_vfs_open(vfs, path, (uint8_t)(M9P_OWRITE | M9P_OTRUNC), deadline_ms, &fd);
    if (rc != 0) {
        return rc;
    }

    rc = get_open_file_qid(vfs, fd, &qid);
    if (rc != 0 || (qid.type & M9P_QID_DIR) != 0u) {
        (void)pwos_cluster_vfs_close(vfs, fd, deadline_ms);
        if (rc == 0) {
            rc = -(int)M9P_ERR_EISDIR;
        }
        set_error(vfs, rc);
        return rc;
    }

    rc = pwos_cluster_vfs_write(vfs, fd, data, len, out_written, deadline_ms);
    close_rc = pwos_cluster_vfs_close(vfs, fd, deadline_ms);
    if (rc != 0) {
        return rc;
    }
    return close_rc;
}

static int list_root(
    pwos_cluster_vfs_t *vfs,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count)
{
    size_t i;
    size_t produced = 0u;

    if (vfs == NULL || out_count == NULL || (max_entries > 0u && entries == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    vfs_lock(vfs);
    for (i = 0u; i < PWOS_CLUSTER_VFS_MAX_ROUTES && produced < max_entries; ++i) {
        if (vfs->routes[i].state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ||
            vfs->routes[i].state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE) {
            continue;
        }
        memset(&entries[produced], 0, sizeof(entries[produced]));
        entries[produced].qid.type = (uint8_t)(M9P_QID_DIR | M9P_QID_VIRTUAL);
        entries[produced].qid.object_id = (uint32_t)(i + 1u);
        entries[produced].flags = (uint8_t)(M9P_STAT_DIR | M9P_STAT_VIRTUAL);
        (void)snprintf(entries[produced].name, sizeof(entries[produced].name), "%s", vfs->routes[i].target);
        ++produced;
    }

    *out_count = produced;
    vfs_unlock(vfs);
    return 0;
}

int pwos_cluster_vfs_list(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t deadline_ms)
{
    uint16_t fd = 0xFFFFu;
    struct m9p_qid qid;
    size_t produced = 0u;
    int rc = 0;
    int close_rc;

    if (vfs == NULL || path == NULL || out_count == NULL ||
        (max_entries > 0u && entries == NULL)) {
        return -(int)M9P_ERR_EINVAL;
    }

    *out_count = 0u;
    if (max_entries == 0u) {
        return 0;
    }
    if (strcmp(path, "/") == 0) {
        return list_root(vfs, entries, max_entries, out_count);
    }

    rc = pwos_cluster_vfs_open(vfs, path, M9P_OREAD, deadline_ms, &fd);
    if (rc != 0) {
        return rc;
    }
    rc = get_open_file_qid(vfs, fd, &qid);
    if (rc != 0 || (qid.type & M9P_QID_DIR) == 0u) {
        (void)pwos_cluster_vfs_close(vfs, fd, deadline_ms);
        if (rc == 0) {
            rc = -(int)M9P_ERR_ENOTDIR;
        }
        set_error(vfs, rc);
        return rc;
    }

    while (produced < max_entries) {
        uint8_t read_buf[M9P_CLIENT_BUFFER_CAP];
        uint16_t read_len = (uint16_t)sizeof(read_buf);
        size_t parsed;

        rc = pwos_cluster_vfs_read(vfs, fd, read_buf, &read_len, deadline_ms);
        if (rc != 0) {
            break;
        }
        if (read_len == 0u) {
            break;
        }

        parsed = m9p_parse_dirents(
            read_buf,
            read_len,
            entries + produced,
            max_entries - produced);
        if (parsed == 0u) {
            rc = -(int)M9P_ERR_EIO;
            set_error(vfs, rc);
            break;
        }
        produced += parsed;
    }

    *out_count = produced;
    close_rc = pwos_cluster_vfs_close(vfs, fd, deadline_ms);
    if (rc != 0) {
        return rc;
    }
    return close_rc;
}

int pwos_cluster_vfs_stat(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    struct m9p_stat *out_stat,
    uint32_t deadline_ms)
{
    pwos_cluster_vfs_route_ref_t route_ref;
    struct m9p_client *client;
    char remote_path[M9P_MAX_PATH_LEN + 1u];
    uint16_t fid = 0u;
    struct m9p_qid qid;
    uint8_t session_index = 0xFFu;
    int rc;

    if (vfs == NULL || path == NULL || out_stat == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }

    if (strcmp(path, "/") == 0) {
        memset(out_stat, 0, sizeof(*out_stat));
        out_stat->qid.type = (uint8_t)(M9P_QID_DIR | M9P_QID_VIRTUAL);
        out_stat->flags = (uint8_t)(M9P_STAT_DIR | M9P_STAT_VIRTUAL);
        (void)snprintf(out_stat->name, sizeof(out_stat->name), "/");
        return 0;
    }

    rc = resolve_path(vfs, path, deadline_ms, &route_ref, remote_path, sizeof(remote_path));
    if (rc != 0) {
        set_error(vfs, rc);
        return rc;
    }

    rc = pwos_session_manager_acquire_client(
        vfs->sessions,
        route_ref.addr,
        route_ref.boot_id,
        deadline_ms,
        &client,
        &session_index);
    if (rc != 0) {
        set_error(vfs, rc);
        return rc;
    }

    rc = m9p_client_walk_path(client, remote_path, &fid, &qid);
    if (rc != 0) {
        pwos_session_manager_release_client(vfs->sessions, session_index);
        set_error(vfs, rc);
        return rc;
    }
    rc = m9p_client_stat(client, fid, out_stat);
    if (rc != 0) {
        (void)m9p_client_clunk(client, fid);
        pwos_session_manager_release_client(vfs->sessions, session_index);
        set_error(vfs, rc);
        return rc;
    }
    rc = m9p_client_clunk(client, fid);
    pwos_session_manager_release_client(vfs->sessions, session_index);
    set_error(vfs, rc);
    return rc;
}

void pwos_cluster_vfs_get_stats(
    const pwos_cluster_vfs_t *vfs,
    pwos_cluster_vfs_stats_t *out_stats)
{
    pwos_cluster_vfs_t *mutable_vfs = (pwos_cluster_vfs_t *)vfs;

    if (vfs == NULL || out_stats == NULL) {
        return;
    }
    vfs_lock(mutable_vfs);
    *out_stats = vfs->stats;
    vfs_unlock(mutable_vfs);
}
