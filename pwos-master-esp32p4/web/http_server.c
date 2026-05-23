#include "http_server.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <string.h>

#include "websocket_shell.h"

static const char *TAG = "web_server";

/* 嵌入的 index.html（由 CMake 的 EMBED_FILES 注入）。 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------ */
/* HTTP GET — 提供单页 Web Shell 前端 UI                                 */
/* ------------------------------------------------------------------ */

static esp_err_t get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving index.html");
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
}

static const httpd_uri_t uri_root = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = get_handler,
};

/* ------------------------------------------------------------------ */
/* WebSocket 处理器 — 与 shell_execute_line 的桥梁                       */
/* ------------------------------------------------------------------ */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* 初始的 WebSocket 升级握手 — 记录新的客户端。 */
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS handshake from fd=%d", fd);
        websocket_shell_client_connected(fd);
        return ESP_OK;
    }

    /* 接收一个 WebSocket 帧。 */
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;

    /* 第一次调用：获取有效的负载长度。 */
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame (len probe) failed: 0x%x", ret);
        return ret;
    }

    if (pkt.len == 0) {
        return ESP_OK; /* 空的 ping 或控制帧 — 忽略 */
    }

    uint8_t *buf = calloc(1, pkt.len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "OOM for WS receive buffer");
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;

    /* 第二次调用：读取实际负载数据。 */
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK) {
        websocket_shell_receive_text((const char *)buf, pkt.len);
    } else {
        ESP_LOGE(TAG, "httpd_ws_recv_frame (data) failed: 0x%x", ret);
    }

    free(buf);
    return ret;
}

static const httpd_uri_t uri_ws = {
    .uri          = "/ws",
    .method       = HTTP_GET,
    .handler      = ws_handler,
    .is_websocket = true,
};

/* ------------------------------------------------------------------ */
/* Socket 关闭回调 — 移除断开连接的客户端                                 */
/* ------------------------------------------------------------------ */

static void on_socket_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ESP_LOGI(TAG, "Socket closed: fd=%d", sockfd);
    websocket_shell_client_disconnected(sockfd);
}

/* ------------------------------------------------------------------ */
/* 服务器启动                                                           */
/* ------------------------------------------------------------------ */

void web_server_start(void)
{
    websocket_shell_init();

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets  = 6;   /* 足够 HTTP 以及几个 WS 客户端使用 */
    cfg.max_uri_handlers  = 4;
    cfg.close_fn          = on_socket_close;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", cfg.server_port);

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_ws);

    /* 与 WebSocket 广播层共享服务器句柄。 */
    websocket_shell_set_server(s_server);

    ESP_LOGI(TAG, "HTTP + WebSocket server running");
}
