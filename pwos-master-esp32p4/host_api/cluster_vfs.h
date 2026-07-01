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

/* route 表与 coordinator 节点表一一对齐，便于从节点快照同步生成 /mcuN。 */
#define PWOS_CLUSTER_VFS_MAX_ROUTES PWOS_HOST_COORDINATOR_MAX_NODES
/* 主机侧最多同时打开 16 个远端 fid。 */
#define PWOS_CLUSTER_VFS_MAX_OPEN 16u
/* 目标名如 "mcu1"，预留 16 字节足够 mcu999 和结尾 NUL。 */
#define PWOS_CLUSTER_VFS_MAX_NAME 16u
/* 0xFF 表示 route 没有可用 mesh 地址，不可发起数据面请求。 */
#define PWOS_CLUSTER_VFS_UNASSIGNED_ADDR 0xFFu

typedef enum {
    /* 槽位未使用。 */
    PWOS_CLUSTER_VFS_ROUTE_EMPTY = 0,
    /* UID 已知但当前不在线，保留 target 名称用于稳定命名。 */
    PWOS_CLUSTER_VFS_ROUTE_OFFLINE,
    /* 在线但还没完成 mini9P attach。 */
    PWOS_CLUSTER_VFS_ROUTE_NEW,
    /* 在线且 mini9P attach 已完成，可直接 open/read/write。 */
    PWOS_CLUSTER_VFS_ROUTE_ATTACHED,
} pwos_cluster_vfs_route_state_t;

typedef struct {
    /* used=0 空闲，used=2 正在 open，used=1 正常打开。 */
    uint8_t used;
    /* 防止同一个 fd 被并发 read/write/close。 */
    uint8_t busy;
    /* 指向 routes[] 的下标。 */
    uint8_t route_index;
    /* file 自身世代号，用来检测 open 期间槽位是否被复用。 */
    uint32_t generation;
    /* 打开时 route.generation 的快照；不一致说明节点身份变化，fd 失效。 */
    uint32_t route_generation;
    /* STM32 mini9P server 侧分配/使用的 fid。 */
    uint16_t remote_fid;
    struct m9p_qid qid;
    /* 打开模式，read/write 前检查权限。 */
    uint8_t mode;
    /* 主机维护的顺序读写偏移，每次成功 I/O 后递增。 */
    uint32_t offset;
} pwos_cluster_vfs_file_t;

typedef struct {
    pwos_cluster_vfs_route_state_t state;
    /* 用户可见路径前缀，如 mcu1/mcu2。 */
    char target[PWOS_CLUSTER_VFS_MAX_NAME];
    /* 当前租约分配到的 mesh 短地址；节点重启后可能保持或变化。 */
    uint8_t addr;
    /* 硬件/逻辑 UID，决定 /mcuN 名称是否沿用。 */
    uint32_t uid[3];
    /* 节点启动代次，变化代表旧 session/fd/job 应失效。 */
    uint32_t boot_id;
    /* route 映射世代号，任何 addr/boot/state 变化都会递增。 */
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
    /* session_manager 是 VFS 发 mini9P 请求的唯一底层入口。 */
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
