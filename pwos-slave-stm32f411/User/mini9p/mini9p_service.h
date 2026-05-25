/**
 * @file mini9p_service.h
 * @brief STM32 slave 侧 Mesh + Mini9P 串口服务组装层。
 *
 * 本模块把 littlefs-backed lfs_vfs、mini9p_server、mesh_node_runtime 和
 * raw mesh UART transport 串成一个可轮询的从机服务。初始化成功后会立刻在
 * 当前 UART 链路上发送一帧携带硬件 UID 的 REGISTER，后续再把发给本机的
 * mesh mini9P 请求分发给本地 server。
 */

#ifndef MINI9P_SERVICE_H
#define MINI9P_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Mesh + Mini9P 串口服务。
 *
 * 初始化顺序为 lfs_vfs -> mini9p_server -> raw mesh UART transport ->
 * mesh_node_runtime，并在 init 成功后自动向当前串口发送 REGISTER。
 *
 * @return 0 成功，负 mesh/transport 错误码失败。
 */
int mini9p_service_init(void);

/**
 * @brief 显式通知“当前 UART 链路已连通”，立即重发一帧 REGISTER。
 *
 * 当板级代码未来具备 link-up 检测时，可在事件回调里调用本接口。
 * 当前 init 默认也会自动发一次 REGISTER。
 *
 * @return 0 成功，负 mesh/transport 错误码失败。
 */
int mini9p_service_notify_link_up(void);

/**
 * @brief 处理至多一个 mesh UART 请求。
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
