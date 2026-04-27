#ifndef PWOS_MASTER_HTTP_SERVER_H
#define PWOS_MASTER_HTTP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化并启动可承载 Web Shell 前端的 HTTP 与 WebSocket 服务器 */
void web_server_start(void);

#ifdef __cplusplus
}
#endif

#endif
