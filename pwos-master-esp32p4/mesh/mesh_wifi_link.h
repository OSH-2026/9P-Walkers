#ifndef PWOS_MASTER_MESH_WIFI_LINK_H
#define PWOS_MASTER_MESH_WIFI_LINK_H

/**
 * @file mesh_wifi_link.h
 * @brief 主机侧 WiFi/局域网 mesh 链路（TCP 监听端）。
 *
 * ESP32-P4 主机不带 WiFi 射频；无线一跳由局域网路由器完成。本模块在
 * 以太网上监听一个 TCP 端口，从机侧 WiFi 透传模块（ESP8266/ESP32 AT
 * 透传，或任何 TCP client）连入后，原始 mesh 帧（'M''H' 封帧字节流）
 * 在该连接上与 UART 完全同构地透传。
 *
 * 与 mesh_host_service 的协作约定：
 * - 接收路径每轮先轮询本链路；命中帧时 ingress port 上报
 *   MESH_HOST_SERVICE_WIFI_INGRESS_PORT。
 * - 发送路径对“已从本链路收到过的 src 地址”单播走 TCP；
 *   对 bootstrap 帧（next_hop = MESH_ADDR_UNASSIGNED）同时向
 *   UART 与本链路广播，保证 ASSIGN 能到达 WiFi 侧的未分配节点。
 *
 * 当前版本只支持一个已连接 client（一个 WiFi 入口）；新连接到来时
 * 替换旧连接。该入口后面可以挂 relay 从机带多个下游节点，因此学习
 * 地址表允许多个地址映射到同一条 TCP 链路。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 默认监听的 TCP 端口。 */
#define MESH_WIFI_LINK_DEFAULT_TCP_PORT 9000u

/** @brief 本链路最多可学习的 mesh 地址数。 */
#define MESH_WIFI_LINK_MAX_ADDRS 8u

/**
 * @brief 启动（或换端口重启）TCP 监听。
 *
 * 已在同一端口监听时为幂等 no-op；端口不同则先停止旧监听。
 *
 * @param[in] tcp_port 监听端口，0 表示使用默认端口。
 * @return 成功返回 0，失败返回负 MESH_ERR_* 错误码。
 */
int mesh_wifi_link_start(uint16_t tcp_port);

/** @brief 停止监听并断开已连接的 client。未启动时为 no-op。 */
int mesh_wifi_link_stop(void);

/** @brief 链路是否处于监听状态。 */
bool mesh_wifi_link_active(void);

/** @brief 该 mesh 地址是否已学习为经由本链路可达。 */
bool mesh_wifi_link_owns_addr(uint8_t mesh_addr);

/**
 * @brief 向已连接的 client 发送一个完整原始 mesh 帧。
 *
 * @return 成功返回 0；无 client 连接时返回 -MESH_ERR_NO_ROUTE（软错误，
 *         调用方可回落 UART 或丢弃等待重传）。
 */
int mesh_wifi_link_send_frame(const uint8_t *tx_data, size_t tx_len);

/**
 * @brief 非阻塞轮询：从 TCP 字节流中提取一个完整 mesh 帧。
 *
 * 同时驱动监听 socket 的 accept。无完整帧时返回 -MESH_ERR_BUSY。
 * 收到完整帧后自动学习帧内 src 地址到本链路的映射。
 */
int mesh_wifi_link_receive_frame(uint8_t *rx_data, size_t rx_cap, size_t *rx_len);

/**
 * @brief 将链路状态格式化为多行文本（监听端口、client、已学地址）。
 *
 * @return 写入的字节数（不含结尾 '\0'），参数非法返回负值。
 */
int mesh_wifi_link_format_status(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_MASTER_MESH_WIFI_LINK_H */
