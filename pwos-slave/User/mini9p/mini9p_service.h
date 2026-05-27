/**
 * @file mini9p_service.h
 * @brief STM32 slave 侧 Mesh + Mini9P 串口服务组装层。
 *
 * 本模块把 mini9p_server、mesh_node_runtime 和 raw mesh UART transport
 * 串成一个可轮询的从机服务。具体本地 VFS/backend 由板级代码注入。
 */

#ifndef MINI9P_SERVICE_H
#define MINI9P_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini9p_server.h"
#include "mesh_uart_transport.h"
#include "../../../pwos-shared/mesh/wifi/mesh_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 板级代码注入给 Mini9P service 的本地 backend。
 *
 * service 只负责 mesh transport 和 server 生命周期；具体 VFS/backend 的
 * 初始化、存储和资源所有权由调用方负责。
 */
struct mini9p_service_backend {
    const struct m9p_server_ops *ops; /**< Mini9P server 调用的 backend 回调表。 */
    void *ops_ctx;                    /**< 传给每个 backend 回调的上下文指针。 */
    uint16_t default_iounit;          /**< backend 默认单次 I/O 大小；0 表示使用 server 默认值。 */
    UART_HandleTypeDef *uart;         /**< 兼容旧版的单 UART 句柄；当 uarts 为空时使用。 */
    UART_HandleTypeDef *const *uarts; /**< 多 UART 模式下的句柄数组。 */
    size_t uart_count;                /**< uarts 数组长度；为 0 时回退到单 UART 模式。 */
    bool wifi_supported;              /**< 当前板级硬件是否启用 Wi-Fi mesh 传输。 */
    const struct mesh_wifi_config *wifi_config; /**< 启用 Wi-Fi 时使用的 mesh_wifi 配置。 */
};

/**
 * @brief 初始化 Mesh + Mini9P 串口服务，并注入本地 Mini9P backend。
 *
 * 初始化顺序为 mini9p_server -> raw mesh transport -> mesh_node_runtime，
 * 并在 init 成功后自动向所有已绑定 UART/Wi-Fi 端口各发送一帧 REGISTER。
 *
 * UART 绑定模式：
 * - 推荐：通过 backend.uarts + backend.uart_count 一次传入当前节点的全部硬件串口；
 * - 兼容：若只填写 backend.uart，则退化为单 UART 模式。
 * - 若 backend.wifi_supported=true，则还会额外挂接一个 Wi-Fi mesh 端口；
 *   当前也允许“0 UART + Wi-Fi”模式。
 *
 * @param backend 板级代码构造好的 Mini9P backend。
 * @return 0 成功，负 mesh/transport 错误码失败。
 */
int mini9p_service_init_with_backend(const struct mini9p_service_backend *backend);

/**
 * @brief 显式通知“当前已绑定链路已连通”，立即向各端口重发 REGISTER。
 *
 * 当板级代码未来具备 link-up 检测时，可在事件回调里调用本接口。
 * 当前 init 默认也会自动发一次 REGISTER。
 *
 * @return 0 成功，负 mesh/transport 错误码失败。
 */
int mini9p_service_notify_link_up(void);

/**
 * @brief 从所有已绑定 UART 中轮询并处理至多一帧 mesh 请求。
 *
 * 当前 poll 入口内部通过 mesh_node_runtime 拉取一帧 raw mesh 数据，再分发到
 * 控制面或本地 mini9P server；在阻塞 HAL UART 超时的情况下，会返回可重试的
 * 负 mesh 错误码。
 *
 * @return 0 成功处理一帧；负 mesh/transport 错误码表示本轮未完成处理。
 */
int mini9p_service_poll_once(void);

#ifdef __cplusplus
}
#endif

#endif
