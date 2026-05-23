#ifndef PWOS_MASTER_WEBSOCKET_SHELL_H
#define PWOS_MASTER_WEBSOCKET_SHELL_H

#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 在 httpd_start() 成功后由 http_server 调用。
 * 存储服务器句柄，用于 httpd_queue_work / 异步 WS 发送。
 */
void websocket_shell_set_server(httpd_handle_t hd);

/* WebSocket 生命周期回调：在握手/套接字关闭时调用。 */
void websocket_shell_client_connected(int fd);
void websocket_shell_client_disconnected(int fd);

/* 在 http_server 初始化时调用以设置内部状态。 */
void websocket_shell_init(void);

/* 接收来自浏览器客户端的文本帧并将其路由到 shell_execute_line。 */
void websocket_shell_receive_text(const char *data, size_t len);

/* 发送格式化消息给所有当前连接的 WebSocket 客户端。 */
void websocket_shell_broadcast(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
