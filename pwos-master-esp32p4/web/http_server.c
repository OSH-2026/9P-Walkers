#include "http_server.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <string.h>
#include "websocket_shell.h"

static const char *TAG = "web_server";

/* 由 CMake EMBED_FILES 生成的嵌入式文件首末指针（自动注入）
 * 将在此获取我们在 4.28 初期编制的 index.html 网页流
 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// ======================== HTTP GET (Serve HTML) ========================
static esp_err_t get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Sending 'index.html'");

    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");

    esp_err_t res = httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed sending index.html (res=%d)", res);
    }
    return res;
}

// ======================== WebSocket Bridge (Shell) ========================
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket Handshake done from client fd: %d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    // 接收到的 WebSocket 数据帧
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // 获取包长
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    
    if (ws_pkt.len) {
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for WS payload");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        
        // 获取实际内容
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            // 调用 Web_Shell WebSocket 桥接器直接喂给终端 `shell_execute_line`
            websocket_shell_receive_text((const char *)ws_pkt.payload, ws_pkt.len);
        } else {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
        }
        
        free(buf);
    }
    return ret;
}

static const httpd_uri_t uri_get = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t ws = {
    .uri        = "/ws",
    .method     = HTTP_GET,
    .handler    = ws_handler,
    .user_ctx   = NULL,
    .is_websocket = true
};

static httpd_handle_t server = NULL;

void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // 增加最大打开连接数支持并行浏览，扩展 URI 处理项以确保 ws 机制平稳
    config.max_open_sockets = 4;
    config.max_uri_handlers = 4;

    ESP_LOGI(TAG, "Starting HTTP server on port: '%d'", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &ws);
        
        websocket_shell_init(); // 初始化对后端 C-shell 的桥接机制
    } else {
        ESP_LOGE(TAG, "Error starting server!");
    }
}