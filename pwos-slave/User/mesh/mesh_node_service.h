/**
 * @file mesh_node_service.h
 * @brief STM32 slave 侧 Mesh 节点服务装配层。
 *
 * mesh_node_service 负责把一个或多个 UART raw mesh transport、节点运行时
 * mesh_node_runtime 和本地 Mini9P server handler 装配成板级可轮询服务。
 *
 * 本层拥有 UART 端口数组和动态 `mesh_addr -> UART port` 映射表。runtime/cluster
 * 仍使用 mesh 地址作为 next_hop；当 send callback 被调用时，service 再把该
 * next_hop 映射到具体 UART 端口。`MESH_NODE_SERVICE_NEIGHBOR_ANY` 是唯一的
 * all-ports broadcast selector，主要用于 bootstrap REGISTER。
 */

#ifndef MESH_NODE_SERVICE_H
#define MESH_NODE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_node_runtime.h"
#include "mesh_uart_transport.h"
#include "mini9p_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单帧 Mini9P 数据面的最大缓冲尺寸。 */
#define MESH_NODE_SERVICE_FRAME_CAP M9P_SERVER_DEFAULT_MSIZE

/** 一个节点服务实例最多管理的 UART mesh 端口数量。 */
#define MESH_NODE_SERVICE_MAX_PORTS 4u

/**
 * @brief 发送到所有已启用 UART 端口的特殊 next_hop selector。
 *
 * 普通单播 next_hop 仍是 mesh 地址，会通过动态 addr-port 表解析到 UART 端口。
 */
#define MESH_NODE_SERVICE_NEIGHBOR_ANY 0xffu

/** 当前 REGISTER 帧通告的节点能力位。 */
#define MESH_NODE_SERVICE_REGISTER_CAPABILITY_BITS 0x0001u

/**
 * @brief 单个 UART mesh 端口配置。
 */
struct mesh_node_service_port_config {
    /** 是否启用该端口。未启用端口不会初始化 transport，也不会参与轮询或广播。 */
    bool enabled;
    /** raw mesh UART transport 的底层配置。 */
    struct mesh_uart_transport_config uart_config;
};

/**
 * @brief STM32 slave mesh service 初始化配置。
 */
struct mesh_node_service_config {
    /** 本地 Mini9P server 帧处理回调，不可为 NULL。 */
    mesh_processer_mini9p_server_handler_fn mini9p_server_handler;
    /** 原样传给 mini9p_server_handler 的上下文。 */
    void *mini9p_server_ctx;
    /** ports 数组中有效配置槽数量，范围为 1..MESH_NODE_SERVICE_MAX_PORTS。 */
    size_t port_count;
    /** UART 端口配置数组。 */
    struct mesh_node_service_port_config ports[MESH_NODE_SERVICE_MAX_PORTS];
};

/**
 * @brief 填充默认 service 配置。
 *
 * 默认配置启用一个 UART 端口，并填入 mesh_uart_transport 的默认参数。
 * 调用方通常还需要设置 ports[0].uart_config.uart、mini9p_server_handler
 * 和 mini9p_server_ctx。
 *
 * @param[out] out_config 要填充的配置结构；为 NULL 时无操作。
 */
void mesh_node_service_get_default_config(struct mesh_node_service_config *out_config);

/**
 * @brief 初始化全局 STM32 slave mesh service。
 *
 * 初始化会建立所有启用的 UART transport，创建内部 mesh_node_runtime，生成本机
 * UID/boot_nonce，并在 runtime 默认配置下立即广播一次 REGISTER。
 * 如果服务已初始化，本函数会先反初始化旧实例。
 *
 * @param[in] config service 配置，必须包含有效 Mini9P handler 和至少一个启用端口。
 * @return 0 表示成功；负的 MESH_ERR_* 表示失败。
 */
int mesh_node_service_init(const struct mesh_node_service_config *config);

/**
 * @brief 反初始化全局 STM32 slave mesh service。
 */
void mesh_node_service_deinit(void);

/**
 * @brief 通知当前链路已恢复或上线，并主动重新发送 REGISTER。
 *
 * @return 0 表示 REGISTER 已提交到底层发送回调；负的 MESH_ERR_* 表示失败。
 */
int mesh_node_service_notify_link_up(void);

/**
 * @brief 轮询一次所有已启用 UART 端口并处理最多一帧 mesh 数据。
 *
 * service 会按 round-robin 顺序读取 UART。成功收到帧后，入口端口会传给 runtime，
 * 用于刷新动态 addr-port 映射和后续转发。
 *
 * @return 0 表示处理了一帧；负的 MESH_ERR_* 表示暂无帧或处理失败。
 */
int mesh_node_service_poll_once(void);

/**
 * @brief 返回全局 service 内部的 runtime 指针。
 *
 * 该接口主要用于调试或测试；未初始化时返回 NULL。
 *
 * @return 已初始化 runtime 指针，或 NULL。
 */
struct mesh_node_runtime *mesh_node_service_runtime(void);

/**
 * @brief 手动学习或更新一个 mesh 地址对应的 UART 端口。
 *
 * runtime 收到带入口端口的帧时会通过内部回调自动调用该逻辑。该公开接口用于
 * 测试、诊断或外部控制面显式修正 addr-port 映射。
 *
 * @param[in] mesh_addr 对端 mesh 地址，不能是 MESH_ADDR_UNASSIGNED。
 * @param[in] port_id UART 端口索引，必须对应已初始化端口。
 * @return 0 表示成功；负的 MESH_ERR_* 表示失败。
 */
int mesh_node_service_learn_addr_port(uint8_t mesh_addr, uint8_t port_id);

/**
 * @brief 将动态 mesh addr -> UART port 表格式化为文本。
 *
 * 输出用于 /sys/routes 调试文件。该表属于 service 层，runtime 路由表中的
 * next_hop 仍保持 mesh 地址语义，实际发送前由本表解析到物理 UART 端口。
 *
 * @param[out] out 输出缓冲。
 * @param[in] out_cap 输出缓冲容量。
 * @return 0 表示成功；负的 MESH_ERR_* 表示失败。
 */
int mesh_node_service_format_addr_ports(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif
