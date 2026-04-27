#ifndef PWOS_MASTER_WEBSOCKET_SHELL_H
#define PWOS_MASTER_WEBSOCKET_SHELL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket 初始处理挂载，在 http_server 中配置时调用
void websocket_shell_init(void);

// 接受从 WebSocket 接收到的客户端字符串数据 (处理函数)
void websocket_shell_receive_text(const char *data, size_t len);

// 提供给 C 程序的广播接口，将数据发送到所有连接的 Web 客户端
void websocket_shell_broadcast(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif