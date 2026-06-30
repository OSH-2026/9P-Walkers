#ifndef PWOS_WEBSOCKET_CONSOLE_H
#define PWOS_WEBSOCKET_CONSOLE_H

#include <stdint.h>

#include "command_service.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t initialized;
    uint8_t worker_started;
    uint8_t clients;
    uint32_t connected;
    uint32_t disconnected;
    uint32_t commands_received;
    uint32_t commands_completed;
    uint32_t commands_failed;
    uint32_t queue_full;
    uint32_t send_failures;
    uint32_t stale_results;
} pwos_websocket_console_status_t;

int pwos_websocket_console_init(
    const pwos_command_service_config_t *command_config);

void pwos_websocket_console_set_server(httpd_handle_t server);

esp_err_t pwos_websocket_console_handle(httpd_req_t *request);

void pwos_websocket_console_client_disconnected(int socket_fd);

void pwos_websocket_console_get_status(
    pwos_websocket_console_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_WEBSOCKET_CONSOLE_H */
