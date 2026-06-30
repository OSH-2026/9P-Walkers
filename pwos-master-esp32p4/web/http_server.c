#include "http_server.h"

#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "websocket_console.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

typedef struct {
    portMUX_TYPE lock;
    httpd_handle_t server;
    pwos_http_server_status_t status;
} pwos_http_server_runtime_t;

static const char *TAG = "pwos_http";
static pwos_http_server_runtime_t g_http = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void record_request(uint32_t *counter)
{
    portENTER_CRITICAL(&g_http.lock);
    ++(*counter);
    portEXIT_CRITICAL(&g_http.lock);
}

static void record_handler_error(esp_err_t error)
{
    if (error == ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&g_http.lock);
    ++g_http.status.handler_errors;
    g_http.status.last_error = (int32_t)error;
    portEXIT_CRITICAL(&g_http.lock);
}

static esp_err_t root_handler(httpd_req_t *request)
{
    esp_err_t error;

    record_request(&g_http.status.root_requests);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    error = httpd_resp_send(
        request,
        (const char *)index_html_start,
        (ssize_t)(index_html_end - index_html_start));
    record_handler_error(error);
    return error;
}

static esp_err_t health_handler(httpd_req_t *request)
{
    esp_err_t error;

    record_request(&g_http.status.health_requests);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    error = httpd_resp_sendstr(request, "ok\n");
    record_handler_error(error);
    return error;
}

static esp_err_t websocket_handler(httpd_req_t *request)
{
    esp_err_t error;

    record_request(&g_http.status.websocket_requests);
    error = pwos_websocket_console_handle(request);
    record_handler_error(error);
    return error;
}

static void socket_close_handler(httpd_handle_t server, int socket_fd)
{
    (void)server;
    pwos_websocket_console_client_disconnected(socket_fd);
    (void)close(socket_fd);
}

int pwos_http_server_start(
    const pwos_command_service_config_t *command_config)
{
    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    static const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
    };
    static const httpd_uri_t websocket_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = websocket_handler,
        .is_websocket = true,
    };
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t error;
    int rc;

    if (command_config == NULL) {
        return (int)ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&g_http.lock);
    if (g_http.status.started != 0u) {
        portEXIT_CRITICAL(&g_http.lock);
        return 0;
    }
    portEXIT_CRITICAL(&g_http.lock);

    rc = pwos_websocket_console_init(command_config);
    if (rc != 0) {
        record_handler_error(ESP_FAIL);
        return rc;
    }

    config.stack_size = 8192u;
    config.max_open_sockets = 6u;
    config.max_uri_handlers = 4u;
    config.lru_purge_enable = true;
    config.close_fn = socket_close_handler;
    error = httpd_start(&g_http.server, &config);
    if (error != ESP_OK) {
        record_handler_error(error);
        return (int)error;
    }

    error = httpd_register_uri_handler(g_http.server, &root_uri);
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(g_http.server, &health_uri);
    }
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(g_http.server, &websocket_uri);
    }
    if (error != ESP_OK) {
        record_handler_error(error);
        (void)httpd_stop(g_http.server);
        g_http.server = NULL;
        return (int)error;
    }

    pwos_websocket_console_set_server(g_http.server);
    portENTER_CRITICAL(&g_http.lock);
    g_http.status.started = 1u;
    g_http.status.port = config.server_port;
    g_http.status.last_error = 0;
    portEXIT_CRITICAL(&g_http.lock);
    ESP_LOGI(TAG, "HTTP/WebSocket server listening on port %u", config.server_port);
    return 0;
}

void pwos_http_server_get_status(pwos_http_server_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    portENTER_CRITICAL(&g_http.lock);
    *out_status = g_http.status;
    portEXIT_CRITICAL(&g_http.lock);
}
