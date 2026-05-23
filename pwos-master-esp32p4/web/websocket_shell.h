#ifndef PWOS_MASTER_WEBSOCKET_SHELL_H
#define PWOS_MASTER_WEBSOCKET_SHELL_H

#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called by http_server after httpd_start() succeeds.
 * Stores the server handle required for httpd_queue_work / async WS sends.
 */
void websocket_shell_set_server(httpd_handle_t hd);

/* WebSocket lifecycle: call on handshake / socket close. */
void websocket_shell_client_connected(int fd);
void websocket_shell_client_disconnected(int fd);

/* Called during http_server init to set up internal state. */
void websocket_shell_init(void);

/* Receive a text frame from a browser client and route it to shell_execute_line. */
void websocket_shell_receive_text(const char *data, size_t len);

/* Send a formatted message to ALL currently connected WebSocket clients. */
void websocket_shell_broadcast(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
