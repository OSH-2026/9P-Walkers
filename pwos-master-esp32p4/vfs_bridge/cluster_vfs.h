/*
cluster_vfs 是轻量级集群 VFS 桥接层，不是完整文件系统；
它负责把统一命名空间路径映射到具体节点上的 mini9P 文件操作。
*/

#ifndef CLUSTER_VFS_H
#define CLUSTER_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_client.h"
#include "../../pwos-shared/mesh/cluster/cluster.h"

/* 当前节点最多维护的静态路由数量。 */
#define CLUSTER_VFS_MAX_ROUTES 8
/* 目标节点名和下一跳节点名的最大长度，包含字符串结尾的 NUL。 */
#define CLUSTER_VFS_MAX_NAME   16
/* cluster_vfs 同时打开的文件/目录数量上限。 */
#define CLUSTER_VFS_MAX_OPEN 16
/* VFS 侧记录的唯一硬件序列号长度，直接复用 mesh REGISTER 的 UID 长度。 */
#define CLUSTER_VFS_UID_LEN MESH_UID_LEN
/* VFS 中“当前无 mesh 地址绑定”的占位值。 */
#define CLUSTER_VFS_UNASSIGNED_ADDR MESH_ADDR_UNASSIGNED

/* VFS 内部只保存 Mini9P 会话状态；节点可达性从 mesh cluster 派生。 */
enum cluster_vfs_m9p_state {
    CLUSTER_VFS_M9P_EMPTY = 0,
    CLUSTER_VFS_M9P_NEW,
    CLUSTER_VFS_M9P_ATTACHED,
};

struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];     /* 最终目标节点，如 "mcu3" */
    struct m9p_client *client;             /* 以该节点为最终目标的 Mini9P client。 */
    uint8_t mesh_addr;                     /* 当前绑定的 mesh 地址；未绑定时为 0xFF。 */
    uint8_t hw_uid[CLUSTER_VFS_UID_LEN];   /* 来自 mesh REGISTER 的唯一硬件序列号。 */
    bool has_hw_uid;                       /* 是否已经收到并保存硬件序列号。 */
    enum cluster_vfs_m9p_state m9p_state;
};

struct cluster_vfs_file {
    bool used;
    uint16_t local_fd;
    struct cluster_vfs_route *route;
    uint16_t remote_fid;
    struct m9p_qid qid;
    uint8_t mode;
    uint32_t offset;
};

/**
 * @brief 初始化 cluster_vfs 的全局静态状态。
 *
 * 清空路由表和本地 open file 表，使后续可以重新添加路由并打开文件。
 * 该函数不初始化任何底层 transport，也不创建 m9p_client。
 *
 * @return 0 表示成功；负 Mini9P 风格错误码表示失败。
 */
int cluster_vfs_init(void);

/**
 * @brief 绑定 mesh cluster 对象。
 *
 * 绑定后，cluster_vfs 可以直接调用共享 mesh cluster 的连通性查询接口，
 * 例如在链路变化后重新判断某个节点是否仍可达。
 *
 * @param mesh_cluster 共享 mesh cluster 对象。
 * @return 0 表示成功；负错误码表示参数非法。
 */
int cluster_vfs_bind_mesh_cluster(struct cluster *mesh_cluster);

/**
 * @brief 处理 mesh 侧“发现新节点/旧节点重连”的事件。
 *
 * 语义：
 * 1. 输入 mesh 地址和硬件序列号。
 * 2. 若该 UID 已有历史映射，则复用已有 target 名称。
 * 3. 若该 UID 首次出现，则自动分配新的节点名（mcuN）。
 * 4. 不论是新节点还是重连节点，9P 状态都会回到 NEW（未 attach）。
 *
 * @param mesh_addr 当前分配给该节点的 mesh 地址。
 * @param hw_uid 节点唯一硬件序列号，长度固定为 CLUSTER_VFS_UID_LEN。
 * @param client 通往该节点的 mini9P client。
 * @param out_target 输出复用/新分配的节点名，可为 NULL。
 * @param out_reused_mapping 输出是否复用了历史名字映射，可为 NULL。
 * @return 0 表示成功；负错误码表示参数非法或路由表已满。
 */
int cluster_vfs_discover_node(
    uint8_t mesh_addr,
    const uint8_t hw_uid[CLUSTER_VFS_UID_LEN],
    struct m9p_client *client,
    const char **out_target,
    bool *out_reused_mapping);

/**
 * @brief 按 mesh 地址把节点标记为离线，并把 9P 状态回退到 NEW。
 *
 * 该接口用于“节点离线/退出”场景：
 * - 保留名字 <-> UID 的历史映射；
 * - 清空当前 mesh 地址绑定；
 * - 把 9P 会话状态回退到未 attach；
 * - 使后续同 UID 重连时可以复用旧名字。
 *
 * @param mesh_addr 当前离线节点的 mesh 地址。
 * @return 0 表示成功；负错误码表示未找到该地址对应的已知节点。
 */
int cluster_vfs_mark_node_offline(uint8_t mesh_addr);

/**
 * @brief 结合绑定的 mesh cluster 重新检查某个节点是否仍可达。
 *
 * 该接口用于链路变化后的“二次确认”：
 * - 若 cluster 判定仍可达，则保持当前 VFS 节点在线状态；
 * - 若 cluster 判定已不可达，则自动执行 mark_node_offline 语义。
 *
 * @param mesh_addr 需要检查的目标地址。
 * @param out_reachable 输出当前是否仍可达。
 * @return 0 表示检查完成；负错误码表示参数非法、未绑定 cluster 或 cluster 查询失败。
 */
int cluster_vfs_refresh_node_from_cluster(uint8_t mesh_addr, bool *out_reachable);

/**
 * @brief 结合绑定的 mesh cluster，重新检查所有已知节点当前是否仍可达。
 *
 * 这个接口用于真正的 mesh runtime：
 * - REGISTER 会让某个 UID 与当前 mesh_addr 建立绑定；
 * - 之后任何 LINK_STATE / ROUTE_UPDATE 变化，都可能让若干节点的可达性发生变化；
 * - runtime 不应只刷新单个地址，而应把当前仍持有 mesh_addr 绑定的全部节点都重检一遍。
 *
 * 当前实现策略：
 * 1. 遍历所有非 EMPTY 且仍持有 mesh_addr 的 route；
 * 2. 对每个 route 调用 cluster_can_reach()；
 * 3. 对已不可达的节点执行 mark_node_offline 语义；
 * 4. 统计本轮新回退为 OFFLINE 的节点个数。
 *
 * @param out_offline_count 输出本轮新回退为离线的节点数，可为 NULL。
 * @return 0 表示检查完成；负错误码表示未绑定 cluster 或 cluster 查询失败。
 */
int cluster_vfs_refresh_all_nodes_from_cluster(size_t *out_offline_count);

/**
 * @brief 对目标节点执行 Mini9P attach。
 *
 * 该函数用于确认 target 对应的最终节点是否能完成 Mini9P 会话建立。
 * 若底层 mesh 需要多跳，中继节点只按 shared cluster 给出的 next_hop
 * 转发请求和应答；Mini9P attach 的语义目标仍然是最终 target 节点。
 *
 * @param target 全局目标节点名。
 * @return 0 表示 attach 成功；负错误码表示路由不存在或 Mini9P attach 失败。
 */
int cluster_vfs_attach(const char *target);

/**
 * @brief 标记目标节点路由为未连接/离线。
 *
 * 初代实现可仅更新本地 route 状态；如果存在该路由上的打开 fd，
 * 应在调用前关闭，或由实现返回 busy 类错误。
 *
 * @param target 全局目标节点名。
 * @return 0 表示成功；负错误码表示路由不存在或仍在使用。
 */
int cluster_vfs_detach(const char *target);

/**
 * @brief 打开集群统一路径下的文件或目录。
 *
 * 解析 `/mcuN/...` 路径，查找目标路由，转换为发送给下一跳的路径，
 * 调用 m9p_client_open_path()，并在本地 open table 中建立
 * local_fd -> remote_fid 的映射。
 *
 * @param path 集群绝对路径，例如 "/mcu1/sys/health"。
 * @param mode Mini9P 打开模式，例如 M9P_OREAD、M9P_OWRITE。
 * @param out_fd 输出本地 fd，供 cluster_vfs_read/write/close 使用。
 * @return 0 表示成功；负错误码表示路径非法、无路由、节点离线或远端打开失败。
 */
int cluster_vfs_open(const char *path, uint8_t mode, uint16_t *out_fd);

/**
 * @brief 从已打开的本地 fd 读取数据。
 *
 * 根据 fd 查找对应 route 和 remote_fid，调用 m9p_client_read()。
 * 成功后可按实际读取字节数推进本地 offset。
 *
 * @param fd cluster_vfs_open() 返回的本地 fd。
 * @param buf 输出缓冲区。
 * @param in_out_len 输入为期望读取字节数，输出为实际读取字节数。
 * @return 0 表示成功；负错误码表示 fd 无效、模式不允许或远端读取失败。
 */
int cluster_vfs_read(uint16_t fd,
                     uint8_t *buf,
                     uint16_t *in_out_len);

/**
 * @brief 向已打开的本地 fd 写入数据。
 *
 * 根据 fd 查找对应 route 和 remote_fid，调用 m9p_client_write()。
 * 成功后可按实际写入字节数推进本地 offset。
 *
 * @param fd cluster_vfs_open() 返回的本地 fd。
 * @param data 待写入数据缓冲区。
 * @param len 期望写入字节数。
 * @param out_written 输出远端确认写入的字节数。
 * @return 0 表示成功；负错误码表示 fd 无效、模式不允许或远端写入失败。
 */
int cluster_vfs_write(uint16_t fd,
                      const uint8_t *data,
                      uint16_t len,
                      uint16_t *out_written);

/**
 * @brief 读取集群路径对应文件的一段内容。
 *
 * 这是 open -> read -> close 的便捷封装，供 Shell/Lua/Web 这类上层模块
 * 直接按路径读取，不需要自己管理本地 fd 和远端 fid。
 *
 * @param path 集群绝对路径，例如 "/mcu1/dev/temp"。
 * @param buf 输出缓冲区。
 * @param in_out_len 输入为期望读取字节数，输出为实际读取字节数。
 * @return 0 表示成功；负错误码表示打开、读取或关闭失败。
 */
int cluster_vfs_read_path(const char *path,
                          uint8_t *buf,
                          uint16_t *in_out_len);

/**
 * @brief 向集群路径对应文件写入一段内容。
 *
 * 这是 open -> write -> close 的便捷封装，供 Shell/Lua/Web 这类上层模块
 * 直接按路径写入，不需要自己管理本地 fd 和远端 fid。
 *
 * @param path 集群绝对路径，例如 "/mcu1/dev/led"。
 * @param data 待写入数据缓冲区。
 * @param len 期望写入字节数。
 * @param out_written 输出远端确认写入的字节数。
 * @return 0 表示成功；负错误码表示打开、写入或关闭失败。
 */
int cluster_vfs_write_path(const char *path,
                           const uint8_t *data,
                           uint16_t len,
                           uint16_t *out_written);

/**
 * @brief 枚举集群路径对应的目录项。
 *
 * 对根目录 "/"，cluster_vfs 会在本地合成已注册节点的挂载点目录项；
 * 对远端路径，则执行 open(dir, OREAD) -> read(dir) -> parse dirent -> close。
 *
 * @param path 集群绝对路径，例如 "/" 或 "/mcu1/dev"。
 * @param entries 输出目录项数组；max_entries 为 0 时可为 NULL。
 * @param max_entries entries 数组最多可容纳的目录项数量。
 * @param out_count 输出实际写入 entries 的目录项数量。
 * @return 0 表示成功；负错误码表示参数非法、打开/读取/关闭失败或目录数据非法。
 */
int cluster_vfs_list(const char *path,
                     struct m9p_dirent *entries,
                     size_t max_entries,
                     size_t *out_count);

/**
 * @brief 查询集群路径对应对象的属性。
 *
 * 解析路径并转发到对应 Mini9P peer，返回远端 m9p_stat。
 * `/` 这类本地虚拟根目录的 stat 可由 cluster_vfs 自己合成。
 *
 * @param path 集群绝对路径。
 * @param out_stat 输出文件或目录属性。
 * @return 0 表示成功；负错误码表示路径非法、无路由或远端 stat 失败。
 */
int cluster_vfs_stat(const char *path,
                     struct m9p_stat *out_stat);

/**
 * @brief 关闭本地 fd 并释放远端 fid。
 *
 * 根据本地 fd 查找 remote_fid，调用 m9p_client_clunk()，随后释放本地
 * open table 表项。即使远端 clunk 失败，实现也应谨慎避免本地 fd 泄漏。
 *
 * @param fd cluster_vfs_open() 返回的本地 fd。
 * @return 0 表示成功；负错误码表示 fd 无效或远端 clunk 失败。
 */
int cluster_vfs_close(uint16_t fd);

#endif /* CLUSTER_VFS_H */
