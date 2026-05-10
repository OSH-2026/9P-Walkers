/**
 * @file mini9p_service.h
 * @brief STM32 slave 侧 Mini9P 串口服务组装层。
 *
 * 本模块把 local_vfs、mini9p_server、mini9p_peer_link 和 uart_transport
 * 串成一个可轮询的从机服务。它不改变协议层或 server 状态机，只负责把
 * “原始 UART 收发”提升为“可同时承接对端主动请求和本端主动请求”的链路层。
 */

#ifndef MINI9P_SERVICE_H
#define MINI9P_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Mini9P 串口服务。
 *
 * 初始化顺序为 local_vfs -> mini9p_server -> 默认 UART transport -> mini9p_peer_link。
 *
 * @return 0 成功，负 Mini9P 错误码失败。
 */
int mini9p_service_init(void);

/**
 * @brief 处理至多一个 Mini9P UART 请求。
 *
 * 当前 poll 入口内部通过 mini9p_peer_link 从 raw UART 帧中分发请求与响应；
 * 在阻塞 HAL UART 超时的情况下，会返回 -M9P_ERR_EAGAIN。
 *
 * @return 0 成功处理一帧；负 Mini9P 错误码表示本轮未完成处理。
 */
int mini9p_service_poll_once(void);

#ifdef __cplusplus
}
#endif

#endif
