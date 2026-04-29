#include "websocket_shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "http_server.h"
#include "shell.h"
#include "esp_log.h"

// 简单 WebSocket 缓冲区
#define WS_BUFFER_SIZE 1024

static const char *TAG = "web_shell";
static char ws_out_buffer[WS_BUFFER_SIZE];

static void websocket_shell_output_sink(void *ctx, const char *text)
{
    (void)ctx;
    web_server_broadcast_text(text);
}

void websocket_shell_init(void)
{
    // 这里未来可以初始化队列或者同步原语，用于向所有活跃连接推送数据
    shell_set_output_hook(websocket_shell_output_sink, NULL);
    ESP_LOGI(TAG, "WebSocket shell integration initialized.");
}

void websocket_shell_receive_text(const char *data, size_t len)
{
    if (len >= WS_BUFFER_SIZE) {
        ESP_LOGW(TAG, "Received frame too large, ignoring.");
        return;
    }
    
    char line[WS_BUFFER_SIZE];
    memcpy(line, data, len);
    line[len] = '\0';
    
    // Web 发送的可能带有换行，在 shell_execute_line 里已经被 trim_line() 处理了
    ESP_LOGI(TAG, "Shell executing WS command: %s", line);
    
    // 执行底层的 Shell 命令 (会涉及到 mini9P, Lua 调度等)
    int rc = shell_execute_line(line);
    
    // shell 自身输出已通过 hook 转发到浏览器，这里只补一条执行状态。
    if (rc != 0) {
        websocket_shell_broadcast("[WS] rc=%d\r\n", rc);
    }
}

void websocket_shell_broadcast(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(ws_out_buffer, WS_BUFFER_SIZE, fmt, args);
    va_end(args);

    web_server_broadcast_text(ws_out_buffer);
}