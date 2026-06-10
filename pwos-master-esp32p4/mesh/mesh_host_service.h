#ifndef PWOS_MASTER_MESH_HOST_SERVICE_H
#define PWOS_MASTER_MESH_HOST_SERVICE_H

/**
 * @file mesh_host_service.h
 * @brief 主机侧原始 mesh 主机服务。
 *
 * 该管理器拥有一个或多个 `mesh_uart_transport` 实例和对应的
 * `mesh_host_runtime` 实例，并提供与
 * `mesh_processer_send_frame_fn` 和 `mesh_processer_receive_frame_fn` 兼容的回调。
 *
 * 发送回调使用调用方提供的 `next_hop` 地址选择出口 UART 端口。
 * 接收回调扫描所有已配置端口，通过单一共享入口流返回帧，
 * 匹配当前 `mesh_host_runtime` 模型。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_host_runtime.h"
#include "mesh_uart_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 一个主机服务最多管理的 UART 端口数。 */
#define MESH_HOST_SERVICE_MAX_PORTS 4u

/** @brief 单端口/默认配置使用的通配邻居地址。 */
#define MESH_HOST_SERVICE_NEIGHBOR_ANY 0xffu

/**
 * @brief WiFi/TCP mesh 链路的虚拟入口端口号。
 *
 * 不占用 UART 槽位 0..MESH_HOST_SERVICE_MAX_PORTS-1，且区别于
 * MESH_PROCESSER_INGRESS_PORT_NONE(0xff)。
 */
#define MESH_HOST_SERVICE_WIFI_INGRESS_PORT 0x80u

/**
 * @brief 一个受管 UART 传输端口的配置。
 *
 * `neighbor_addr` 是可通过该 UART 到达的直接下一跳 mesh 地址，
 * 不是路由流量的最终目的地址。
 */
struct mesh_host_service_port_config {
    /** 该槽位是否应被初始化并使用。 */
    bool enabled;

    /** 该端口的直接下一跳 mesh 地址，或 `MESH_HOST_SERVICE_NEIGHBOR_ANY`。 */
    uint8_t neighbor_addr;

    /** 该端口底层 UART 传输配置。 */
    struct mesh_uart_transport_config uart_config;
};

/**
 * @brief mesh 主机服务实例的配置。
 */
struct mesh_host_service_config {
    /** `ports` 中应被考虑的条目数量。 */
    size_t port_count;

    /** 固定大小的端口配置表。 */
    struct mesh_host_service_port_config ports[MESH_HOST_SERVICE_MAX_PORTS];
};

/**
 * @brief 一个受管 UART 端口的运行时状态。
 */
struct mesh_host_service_port {
    /** 该端口是否已成功初始化。 */
    bool initialized;

    /** 该端口的直接下一跳 mesh 地址。 */
    uint8_t neighbor_addr;

    /** 该端口拥有的 UART 传输实例。 */
    struct mesh_uart_transport transport;
};

/**
 * @brief 主机侧 mesh 主机服务的运行时状态。
 *
 * 该对象拥有所有已初始化的 `mesh_uart_transport` 实例和一个主机侧
 * `mesh_host_runtime` 实例。
 * 它可以是静态/全局对象，或嵌入另一个长期运行的运行时对象中。
 */
struct mesh_host_service {
    /** 管理器是否已成功初始化。 */
    bool initialized;

    /** 该管理器考虑的端口槽位数。 */
    size_t port_count;

    /** 下一轮轮询接收扫描的起始槽位。 */
    size_t next_rx_index;

    /** 服务拥有的主机侧 mesh runtime。 */
    struct mesh_host_runtime runtime;

    /** 受管端口运行时状态表。 */
    struct mesh_host_service_port ports[MESH_HOST_SERVICE_MAX_PORTS];
};

/**
 * @brief 用安全的单端口默认配置填充管理器配置。
 *
 * 默认配置启用一个 UART 端口，使用
 * `mesh_uart_transport_get_default_config()`，并将其映射到
 * `MESH_HOST_SERVICE_NEIGHBOR_ANY`。
 *
 * @param[out] out_config 要填充的配置对象。NULL 会被忽略。
 */
void mesh_host_service_get_default_config(struct mesh_host_service_config *out_config);

/**
 * @brief 初始化一个 mesh 主机服务实例。
 *
 * 初始化会验证端口表、拒绝重复的具体 `neighbor_addr` 值、
 * 初始化所有已启用的 UART 传输，并准备接收轮询游标。
 *
 * @param[out] manager 要初始化的管理器实例。
 * @param[in] config 管理器配置。
 * @return 成功返回 0，失败返回负 `MESH_ERR_*` 错误码。
 */
int mesh_host_service_init(
    struct mesh_host_service *manager,
    const struct mesh_host_service_config *config);

/**
 * @brief 去初始化一个 mesh 主机服务实例。
 *
 * 所有已初始化的 UART 端口都会被去初始化，管理器对象被重置。
 *
 * @param[in,out] manager 管理器实例，NULL 为无操作。
 */
void mesh_host_service_deinit(struct mesh_host_service *manager);

/**
 * @brief 初始化进程级默认主机服务。
 *
 * 默认管理器使用单端口默认配置，并装配其持有的默认 runtime。
 *
 * @return 成功返回 0，失败返回负 `MESH_ERR_*` 错误码。
 */
int mesh_host_service_init_default(void);

/** @brief 去初始化进程级默认主机服务。 */
void mesh_host_service_deinit_default(void);

/** @brief 返回进程级默认主机服务。 */
struct mesh_host_service *mesh_host_service_default(void);

/** @brief 返回进程级默认主机服务持有的 runtime。 */
struct mesh_host_runtime *mesh_host_service_default_runtime(void);

/**
 * @brief 启动默认主机服务的后台轮询任务。
 *
 * 默认路径会初始化 cluster_config、UART host service 和 host runtime；
 * ESP 平台会创建后台任务持续轮询 runtime，主机测试环境返回 -M9P_ERR_ENOTSUP。
 */
int mesh_host_service_start_default_task(void);

/**
 * @brief 通过选定的出口端口发送一个完整的原始 mesh 帧。
 *
 * 只有一个已初始化端口时，所有帧都通过该端口发送。
 * 多个端口时，`next_hop` 必须匹配一个已配置的具体 `neighbor_addr`。
 *
 * @param[in] transport_ctx 作为不透明上下文传递的 `struct mesh_host_service *`。
 * @param[in] next_hop 集群路由选择的直接下一跳 mesh 地址。
 * @param[in] tx_data 完整的原始 mesh 帧字节。
 * @param[in] tx_len `tx_data` 中的字节数。
 * @return 成功返回 0，失败返回负 `MESH_ERR_*` 错误码。
 */
int mesh_host_service_send_frame(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len);

/**
 * @brief 从任意已配置的入口端口接收一个完整的原始 mesh 帧。
 *
 * 管理器以轮询顺序扫描已初始化端口，返回第一个成功接收的帧。
 * 软错误（无帧/忙）允许扫描继续到后续端口。
 *
 * @param[in] transport_ctx 作为不透明上下文传递的 `struct mesh_host_service *`。
 * @param[out] rx_data 接收原始 mesh 帧字节的缓冲区。
 * @param[in] rx_cap `rx_data` 的容量。
 * @param[out] rx_len 已接收的字节数；扫描前被设为 0。
 * @return 成功返回 0，失败返回负 `MESH_ERR_*` 错误码。
 */
int mesh_host_service_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len,
    uint8_t *out_ingress_port);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_MASTER_MESH_HOST_SERVICE_H */
