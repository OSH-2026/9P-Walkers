/**
 * @file mini9p_service.h
 * @brief STM32 slave 侧 Mini9P 串口服务组装层。
 *
 * 本模块把 local_vfs、mini9p_server 和 uart_transport 串成一个可轮询的
 * 从机服务。它不改变协议层或 server 状态机，只提供上板联调所需的
 * init/poll 入口。
 */

#ifndef MINI9P_SERVICE_H
#define MINI9P_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Mini9P 串口服务。
 *
 * 初始化顺序为 local_vfs -> mini9p_server -> 默认 UART transport。
 *
 * @return 0 成功，负 Mini9P 错误码失败。
 */
int mini9p_service_init(void);

/**
 * @brief 处理至多一个 Mini9P UART 请求。
 *
 * 当前 transport 使用阻塞 HAL UART 接收，超时时会返回 -M9P_ERR_EAGAIN。
 *
 * @return 0 成功处理一帧；负 Mini9P 错误码表示本轮未完成处理。
 */
int mini9p_service_poll_once(void);

#ifdef __cplusplus
}
#endif

#endif
