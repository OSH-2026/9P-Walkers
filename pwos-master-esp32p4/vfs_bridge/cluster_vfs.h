/*
cluster_vfs 是轻量级集群 VFS 桥接层，不是完整文件系统；
它负责把统一命名空间路径映射到具体节点上的 mini9P 文件操作。
*/

#ifndef CLUSTER_VFS_H
#define CLUSTER_VFS_H

#include <stdbool.h>
#include <stdint.h>

#include "mini9p_client.h"

/* 当前节点最多维护的静态路由数量。 */
#define CLUSTER_VFS_MAX_ROUTES 8
/* 目标节点名和下一跳节点名的最大长度，包含字符串结尾的 NUL。 */
#define CLUSTER_VFS_MAX_NAME   16
/* cluster_vfs 同时打开的文件/目录数量上限。 */
#define CLUSTER_VFS_MAX_OPEN 16

enum cluster_vfs_route_state {
    CLUSTER_VFS_ROUTE_EMPTY = 0,
    CLUSTER_VFS_ROUTE_READY,
    CLUSTER_VFS_ROUTE_ATTACHED,
    CLUSTER_VFS_ROUTE_OFFLINE,
};

struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];     /* 最终目标节点，如 "mcu3" */
    char next_hop[CLUSTER_VFS_MAX_NAME];   /* 下一跳节点，如 "mcu1" */
    struct m9p_client *client;             /* 通往 next_hop 的 Mini9P client */
    enum cluster_vfs_route_state state;
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
 * @brief 添加一条直连路由。add_route 只注册不验证，attach 才确认节点在线。
 *
 * 等价于 target == next_hop 的路由，例如把 `/mcu1/...` 映射到
 * `client` 所代表的直连 Mini9P peer。client 必须由调用者提前通过
 * m9p_client_init() 初始化。
 *
 * @param target 全局目标节点名，例如 "mcu1"，不包含前导 '/'。
 * @param client 通往该目标节点的 Mini9P client。
 * @return 0 表示成功；负错误码表示参数非法、路由重复或路由表已满。
 */
int cluster_vfs_add_direct(const char *target,
                           struct m9p_client *client);

/**
 * @brief 添加一条静态路由。
 *
 * 用于去中心化或中继场景。target 是最终目标节点，next_hop 是当前节点
 * 应该发送到的下一跳。直连路由可表示为 target == next_hop。
 *
 * MVP 中，target != next_hop 时可采用全局路径转发：把原始
 * `/target/...` 路径发送给下一跳 router。
 *
 * @param target 全局目标节点名，例如 "mcu3"。
 * @param next_hop 下一跳节点名，例如 "mcu1"。
 * @param client 通往 next_hop 的 Mini9P client。
 * @return 0 表示成功；负错误码表示参数非法、路由重复或路由表已满。
 */
int cluster_vfs_add_route(const char *target,
                          const char *next_hop,
                          struct m9p_client *client);

/**
 * @brief 移除一条目标节点路由。
 *
 * 如果该 target 仍有打开的本地 fd，初代实现建议返回 busy 类错误，
 * 避免留下失效的 remote fid。
 *
 * @param target 要移除的目标节点名。
 * @return 0 表示成功；负错误码表示未找到、仍在使用或参数非法。
 */
int cluster_vfs_remove_route(const char *target);

/**
 * @brief 对目标节点对应的下一跳执行 Mini9P attach。
 *
 * 该函数用于确认路由背后的 peer 是否在线，并建立根 fid 会话。
 * 对间接路由而言，attach 的是 next_hop 对应的 client，而不是最终
 * target 节点。
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
