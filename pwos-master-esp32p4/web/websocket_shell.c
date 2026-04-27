#include "websocket_shell.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "shell.h"
#include "esp_log.h"

// 简单 WebSocket 缓冲区
#define WS_BUFFER_SIZE 1024

static const char *TAG = "web_shell";
static char ws_out_buffer[WS_BUFFER_SIZE];

void websocket_shell_init(void)
{
    // 这里未来可以初始化队列或者同步原语，用于向所有活跃连接推送数据
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
    
    // 简易回应，证明命令到达底层。实际应该 Hook printf 或者重定向标准输出流
    if (rc == 0) {
        websocket_shell_broadcast("[WS] Executed: '%s' SUCCESS\r\n", line);
    } else {
        websocket_shell_broadcast("[WS] Executed: '%s' FAILED (rc=%d)\r\n", line, rc);
    }
}

// 模拟的群发回调（依赖于 http_server.c 中的活跃连接句柄）
// 当前实现仅作为占位，打印至控制台。在后续的完善中会调用 httpd_ws_send_frame_async 发送到各浏览器
void websocket_shell_broadcast(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(ws_out_buffer, WS_BUFFER_SIZE, fmt, args);
    va_end(args);
    
    // TODO: 实现由 ESP-IDF HTTPD WS 发送异步帧到 Socket Client
    printf("TODO_WS_BROADCAST: %s", ws_out_buffer);
}