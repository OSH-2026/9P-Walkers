#ifndef PWOS_CLUSTER_VFS_H
#define PWOS_CLUSTER_VFS_H

#include <stddef.h>
#include <stdint.h>

#include "host_coordinator.h"
#include "mini9p_protocol.h"
#include "session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_CLUSTER_VFS_MAX_ROUTES PWOS_HOST_COORDINATOR_MAX_NODES
#define PWOS_CLUSTER_VFS_MAX_OPEN 16u
#define PWOS_CLUSTER_VFS_MAX_NAME 16u
#define PWOS_CLUSTER_VFS_UNASSIGNED_ADDR 0xFFu

typedef enum {
    PWOS_CLUSTER_VFS_ROUTE_EMPTY = 0,
    PWOS_CLUSTER_VFS_ROUTE_OFFLINE,
    PWOS_CLUSTER_VFS_ROUTE_NEW,
    PWOS_CLUSTER_VFS_ROUTE_ATTACHED,
} pwos_cluster_vfs_route_state_t;

typedef struct {
    uint8_t used;
    uint8_t busy;
    uint8_t route_index;
    uint32_t generation;
    uint32_t route_generation;
    uint16_t remote_fid;
    struct m9p_qid qid;
    uint8_t mode;
    uint32_t offset;
} pwos_cluster_vfs_file_t;

typedef struct {
    pwos_cluster_vfs_route_state_t state;
    char target[PWOS_CLUSTER_VFS_MAX_NAME];
    uint8_t addr;
    uint32_t uid[3];
    uint32_t boot_id;
    uint32_t generation;
} pwos_cluster_vfs_route_t;

typedef void (*pwos_cluster_vfs_lock_fn)(void *ctx);

typedef struct {
    void *lock_ctx;
    /* 只保护 routes/files/stats 元数据，不允许跨链路 I/O 持锁。 */
    pwos_cluster_vfs_lock_fn lock;
    pwos_cluster_vfs_lock_fn unlock;
} pwos_cluster_vfs_config_t;

typedef struct {
    uint32_t sync_count;
    uint32_t discovered;
    uint32_t rediscovered;
    uint32_t detached;
    uint32_t attach_ok;
    uint32_t attach_fail;
    uint32_t open_ok;
    uint32_t open_fail;
    uint32_t read_ok;
    uint32_t read_fail;
    uint32_t write_ok;
    uint32_t write_fail;
    uint32_t close_ok;
    uint32_t close_fail;
    int32_t last_error;
} pwos_cluster_vfs_stats_t;

typedef struct {
    pwos_session_manager_t *sessions;
    pwos_cluster_vfs_route_t routes[PWOS_CLUSTER_VFS_MAX_ROUTES];
    pwos_cluster_vfs_file_t files[PWOS_CLUSTER_VFS_MAX_OPEN];
    pwos_cluster_vfs_stats_t stats;
    pwos_cluster_vfs_config_t config;
    uint32_t next_route_generation;
    uint32_t next_file_generation;
} pwos_cluster_vfs_t;

int pwos_cluster_vfs_init(
    pwos_cluster_vfs_t *vfs,
    pwos_session_manager_t *sessions);

int pwos_cluster_vfs_init_with_config(
    pwos_cluster_vfs_t *vfs,
    pwos_session_manager_t *sessions,
    const pwos_cluster_vfs_config_t *config);

int pwos_cluster_vfs_sync_from_coordinator(
    pwos_cluster_vfs_t *vfs,
    const pwos_host_coordinator_t *coordinator);

int pwos_cluster_vfs_get_route(
    pwos_cluster_vfs_t *vfs,
    size_t index,
    pwos_cluster_vfs_route_t *out_route);

int pwos_cluster_vfs_attach(
    pwos_cluster_vfs_t *vfs,
    const char *target,
    uint32_t deadline_ms);

int pwos_cluster_vfs_open(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    uint8_t mode,
    uint32_t deadline_ms,
    uint16_t *out_fd);

int pwos_cluster_vfs_read(
    pwos_cluster_vfs_t *vfs,
    uint16_t fd,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

int pwos_cluster_vfs_write(
    pwos_cluster_vfs_t *vfs,
    uint16_t fd,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms);

int pwos_cluster_vfs_close(
    pwos_cluster_vfs_t *vfs,
    uint16_t fd,
    uint32_t deadline_ms);

int pwos_cluster_vfs_read_path(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len,
    uint32_t deadline_ms);

int pwos_cluster_vfs_write_path(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    const uint8_t *data,
    uint16_t len,
    uint16_t *out_written,
    uint32_t deadline_ms);

int pwos_cluster_vfs_list(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t deadline_ms);

int pwos_cluster_vfs_stat(
    pwos_cluster_vfs_t *vfs,
    const char *path,
    struct m9p_stat *out_stat,
    uint32_t deadline_ms);

void pwos_cluster_vfs_get_stats(
    const pwos_cluster_vfs_t *vfs,
    pwos_cluster_vfs_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_CLUSTER_VFS_H */
