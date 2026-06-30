#ifndef PWOS_HTTP_SERVER_H
#define PWOS_HTTP_SERVER_H

#include <stdint.h>

#include "command_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t started;
    uint16_t port;
    uint32_t root_requests;
    uint32_t health_requests;
    uint32_t websocket_requests;
    uint32_t handler_errors;
    int32_t last_error;
} pwos_http_server_status_t;

int pwos_http_server_start(
    const pwos_command_service_config_t *command_config);

void pwos_http_server_get_status(pwos_http_server_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HTTP_SERVER_H */
