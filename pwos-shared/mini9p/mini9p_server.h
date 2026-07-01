/**
 * @file mini9p_server.h
 * @author hb (huobin92@gmail.com)
 * @brief Mini9P 协议服务端（STM32 slave 端）
 *
 * 接收 master 发来的 Mini9P 帧，解析请求类型，分发到虚拟文件树的后端回调，
 * 构造响应帧返回。设计为无动态内存分配，适配 RTOS 嵌入式环境。
 *
 * @version 0.1
 * @date 2026-04-23
 *
 * @copyright Copyright (c) 2026
 */

#ifndef MINI9P_SERVER_H
#define MINI9P_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"

/** @defgroup m9p_server_limits 资源上限与协议默认值
 *  @{
 */

/** fid 表上限，每条含路径占 ~256B，16 条 ≈ 4KB RAM */
#define M9P_SERVER_MAX_FIDS 16u

/** read 暂存缓冲区大小（字节），匹配常见外设数据帧 */
#define M9P_SERVER_IO_BUFFER_CAP 256u

/** 默认最大消息尺寸（字节），115200 baud UART 下约 45ms */
#define M9P_SERVER_DEFAULT_MSIZE 512u

/** 默认最大并发请求数，1 表示 stop-and-wait 模式 */
#define M9P_SERVER_DEFAULT_MAX_INFLIGHT 1u

/** 根目录 fid，由客户端在 Tattach 中指定 */
#define M9P_SERVER_ROOT_FID 0u

/** @} */

/** @defgroup m9p_server_perms 文件权限位（stat.perm 字段）
 *  @{
 */

/** 读权限 */
#define M9P_SERVER_PERM_READ 0x01u

/** 写权限 */
#define M9P_SERVER_PERM_WRITE 0x02u

/** 执行权限 */
#define M9P_SERVER_PERM_EXEC 0x04u

/** @} */

/**
 * @brief server 访问本地资源的回调表
 *
 * 回调返回 0 表示成功，返回负 mini9p 错误码（如 -M9P_ERR_ENOENT）表示失败。
 * stat 是 walk/open/stat 的基础查询入口；open/read/write/clunk 可按节点能力选择实现。
 * 未实现的回调可置 NULL，server 会返回 M9P_ERR_ENOTSUP。
 */
struct m9p_server_ops {
    /**
     * @brief 查询文件/目录属性
     *
     * @param[in]  ctx       ops 上下文
     * @param[in]  path      绝对路径（以 '/' 开头）
     * @param[out] out_stat  输出属性
     * @return 0 成功，负错误码失败
     */
    int (*stat)(void *ctx, const char *path, struct m9p_stat *out_stat);

    /**
     * @brief 打开文件，返回 qid 和 iounit
     *
     * @param[in]  ctx       ops 上下文
     * @param[in]  path      绝对路径
     * @param[in]  mode      打开模式（M9P_OREAD / M9P_OWRITE / M9P_ORDWR / M9P_OTRUNC）
     * @param[out] out_qid   输出 qid
     * @param[out] out_iounit 输出建议的单次 I/O 大小，0 表示使用默认值
     * @return 0 成功，负错误码失败
     */
    int (*open)(void *ctx, const char *path, uint8_t mode, struct m9p_qid *out_qid, uint16_t *out_iounit);

    /**
     * @brief 从文件读取数据
     *
     * @param[in]  ctx       ops 上下文
     * @param[in]  path      绝对路径
     * @param[in]  offset    起始偏移
     * @param[in]  mode      打开模式
     * @param[out] out_data  输出缓冲区
     * @param[in]  out_cap   缓冲区容量
     * @param[out] out_count 实际读取字节数
     * @return 0 成功，负错误码失败
     */
    int (*read)(void *ctx,
                const char *path,
                uint32_t offset,
                uint8_t mode,
                uint8_t *out_data,
                uint16_t out_cap,
                uint16_t *out_count);

    /**
     * @brief 向文件写入数据
     *
     * @param[in]  ctx       ops 上下文
     * @param[in]  path      绝对路径
     * @param[in]  offset    起始偏移
     * @param[in]  mode      打开模式
     * @param[in]  data      待写入数据
     * @param[in]  count     待写入字节数
     * @param[out] out_count 实际写入字节数
     * @return 0 成功，负错误码失败
     */
    int (*write)(void *ctx,
                 const char *path,
                 uint32_t offset,
                 uint8_t mode,
                 const uint8_t *data,
                 uint16_t count,
                 uint16_t *out_count);

    /**
     * @brief 关闭 fid 对应的文件
     *
     * @param[in] ctx       ops 上下文
     * @param[in] path      绝对路径
     * @param[in] was_open  该 fid 是否曾被 open 过
     * @return 0 成功，负错误码失败
     */
    int (*clunk)(void *ctx, const char *path, bool was_open);
};

/**
 * @brief server 初始化配置
 *
 * 传给 m9p_server_init()，字段为 0 时使用编译期默认值。
 */
struct m9p_server_config {
    const struct m9p_server_ops *ops;  /**< 后端回调表，不可为 NULL */
    void *ops_ctx;                     /**< 传递给每个回调的 ctx 参数 */
    /*
     * 以下协商参数会在 RATTACH 中返回给主机侧 m9p_client，
     * C 角色答辩时可说明主机 open/read 的单次大小受这些值约束。
     */
    uint16_t max_msize;                /**< 可协商的最大消息尺寸，0 使用 M9P_SERVER_DEFAULT_MSIZE */
    uint8_t max_fids;                  /**< 最大活跃 fid 数，0 使用 M9P_SERVER_MAX_FIDS */
    uint8_t max_inflight;              /**< 最大并发请求，0 使用 M9P_SERVER_DEFAULT_MAX_INFLIGHT */
    uint16_t default_iounit;           /**< 默认 iounit，0 使用 M9P_SERVER_DEFAULT_IOUNIT */
    uint32_t feature_bits;             /**< 支持的特性位（M9P_FEATURE_*） */
    struct m9p_qid root_qid;           /**< 根目录 qid，全零时使用默认值 */
};

/**
 * @brief 一个活跃 fid 的服务端状态
 */
struct m9p_server_fid {
    bool in_use;                          /**< 是否已分配 */
    bool open;                            /**< 是否已打开（经过 Topen） */
    /*
     * fid 是主机指定的远端句柄号。cluster_vfs 保存 remote_fid，
     * 后续 read/write/close 都用这个编号访问同一远端对象。
     */
    uint16_t fid;                         /**< 客户端指定的 fid 值 */
    uint8_t mode;                         /**< 打开模式（M9P_OREAD 等） */
    uint16_t iounit;                      /**< 协商后的单次 I/O 大小 */
    struct m9p_qid qid;                   /**< 该 fid 对应的 qid */
    char path[M9P_MAX_PATH_LEN + 1u];    /**< 解析后的绝对路径 */
};

/**
 * @brief mini9p server 会话状态
 *
 * 包含一次 attach 会话所需的全部状态：协商参数、fid 表、I/O 缓冲区。
 * 由 m9p_server_init() 初始化，m9p_server_reset_session() 可清除会话。
 */
struct m9p_server {
    const struct m9p_server_ops *ops;               /**< 后端回调表 */
    void *ops_ctx;                                  /**< 回调上下文 */
    bool attached;                                  /**< 是否已完成 Tattach */
    uint16_t negotiated_msize;                      /**< 协商后的消息尺寸 */
    uint16_t max_msize;                             /**< 允许的最大消息尺寸 */
    uint16_t default_iounit;                        /**< 默认 iounit ，ROPEN返回需要iounit,这里设置默认值*/
    uint16_t root_fid;                              /**< 根目录 fid */
    uint8_t max_fids;                               /**< 最大活跃 fid 数 */
    uint8_t max_inflight;                           /**< 最大并发请求 */
    uint32_t feature_bits;                          /**< 支持的特性位 */
    struct m9p_qid root_qid;                        /**< 根目录 qid */
    struct m9p_server_fid fids[M9P_SERVER_MAX_FIDS]; /**< fid 表 */
    uint8_t io_buffer[M9P_SERVER_IO_BUFFER_CAP];    /**< read 暂存缓冲区 */
};

/**
 * @brief 获取默认配置
 *
 * @param[out] out_config 输出默认配置，所有字段填入编译期默认值
 */
void m9p_server_get_default_config(struct m9p_server_config *out_config);

/**
 * @brief 初始化 server
 *
 * @param[out] server  待初始化的 server 结构体
 * @param[in]  config  初始化配置，NULL 时使用全部默认值
 * @return 0 成功，负错误码失败
 */
int m9p_server_init(struct m9p_server *server, const struct m9p_server_config *config);

/**
 * @brief 重置会话状态
 *
 * 释放所有活跃 fid，清除 attached 标志，恢复默认 msize。
 * 用于连接断开后重新 attach 前的清理。
 *
 * @param[in,out] server 要重置的 server
 */
void m9p_server_reset_session(struct m9p_server *server);

/**
 * @brief 处理一帧请求并生成响应帧
 *
 * server 的唯一入口函数。解码请求帧，根据 type 分发到对应 handler，
 * 构造响应帧写入 response_data。所有 master→slave 的请求均经过此函数。
 *
 * @param[in]  server_ctx    指向 m9p_server 的 void 指针
 * @param[in]  request_data  原始请求帧字节
 * @param[in]  request_len   请求帧长度
 * @param[out] response_data 响应帧输出缓冲区
 * @param[in]  response_cap  响应缓冲区容量
 * @param[out] response_len  实际响应帧长度
 * @return 0 成功，负错误码失败（同时会在 response 中填充 Rerror）
 */
int m9p_server_handle_frame(void *server_ctx,
                            const uint8_t *request_data,
                            size_t request_len,
                            uint8_t *response_data,
                            size_t response_cap,
                            size_t *response_len);

#endif
