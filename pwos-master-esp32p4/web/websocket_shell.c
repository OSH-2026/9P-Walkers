#include "websocket_shell.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "shell.h"

#define WS_BROADCAST_CAP 1024
#define WS_CMD_MAX_LEN   256
#define MAX_WS_CLIENTS   4
#define WS_CMD_QUEUE_LEN  4
#define WS_SHELL_STACK    12288
#define WS_SHELL_PRIORITY 4

static const char *TAG = "web_shell";

/* httpd_start() 后传入的服务器句柄；httpd_queue_work 需要用到。 */
static httpd_handle_t s_server = NULL;

/* 活跃的 WebSocket 客户端套接字 FD（通过自旋锁保护）。 */
static int           s_client_fds[MAX_WS_CLIENTS];
static int           s_client_count = 0;
static portMUX_TYPE  s_client_lock  = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    char line[WS_CMD_MAX_LEN];
} ws_cmd_item_t;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t  s_shell_task = NULL;

/* ------------------------------------------------------------------ */
/* 客户端记账管理                                                       */
/* ------------------------------------------------------------------ */

void websocket_shell_set_server(httpd_handle_t hd)
{
    s_server = hd;
}

void websocket_shell_client_connected(int fd)
{
    int total = 0;
    bool accepted = false;

    taskENTER_CRITICAL(&s_client_lock);
    if (s_client_count < MAX_WS_CLIENTS) {
        s_client_fds[s_client_count++] = fd;
        total = s_client_count;
        accepted = true;
    }
    taskEXIT_CRITICAL(&s_client_lock);

    if (accepted) {
        ESP_LOGI(TAG, "WS client connected fd=%d (total=%d)", fd, total);
    } else {
        ESP_LOGW(TAG, "WS client table full, rejecting fd=%d", fd);
    }
}

void websocket_shell_client_disconnected(int fd)
{
    int total = 0;
    bool removed = false;

    taskENTER_CRITICAL(&s_client_lock);
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) {
            /* 交换删除以实现不保留顺序的 O(1) 删除。 */
            s_client_fds[i] = s_client_fds[--s_client_count];
            total = s_client_count;
            removed = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_client_lock);

    if (removed) {
        ESP_LOGI(TAG, "WS client disconnected fd=%d (total=%d)", fd, total);
    }
}

/* ------------------------------------------------------------------ */
/* 异步发送机制                                                         */
/* ------------------------------------------------------------------ */

/*
 * 每个客户端每次发送时分配到堆上的工作项。
 * httpd_queue_work 在 httpd 任务中运行其回调函数，在这个任务里
 * 调用 httpd_ws_send_frame_async 是安全的。
 */
typedef struct {
    int    fd;
    size_t len;
    uint8_t data[]; /* 柔性数组：有效负载在结构体之后 */
} ws_send_work_t;

static void ws_send_work_fn(void *arg)
{
    ws_send_work_t *work = (ws_send_work_t *)arg;

    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = work->data,
        .len     = work->len,
        .final   = true,
    };

    esp_err_t err = httpd_ws_send_frame_async(s_server, work->fd, &frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS send failed fd=%d err=0x%x, removing client", work->fd, err);
        websocket_shell_client_disconnected(work->fd);
    }
    free(work);
}

/* 通过 httpd_queue_work 向每个已连接的客户端发送原始字节。 */
static void ws_send_raw_to_all(const char *data, size_t len)
{
    if (s_server == NULL || len == 0) {
        return;
    }

    /* 在持有自旋锁时对客户端列表进行快照。 */
    taskENTER_CRITICAL(&s_client_lock);
    int count = s_client_count;
    int fds[MAX_WS_CLIENTS];
    memcpy(fds, s_client_fds, (size_t)count * sizeof(int));
    taskEXIT_CRITICAL(&s_client_lock);

    for (int i = 0; i < count; i++) {
        ws_send_work_t *work = malloc(sizeof(ws_send_work_t) + len + 1);
        if (work == NULL) {
            ESP_LOGE(TAG, "OOM allocating WS send work for fd=%d", fds[i]);
            continue;
        }
        work->fd  = fds[i];
        work->len = len;
        memcpy(work->data, data, len);
        work->data[len] = '\0';

        esp_err_t err = httpd_queue_work(s_server, ws_send_work_fn, work);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "httpd_queue_work failed fd=%d err=0x%x", fds[i], err);
            free(work);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Shell 输出钩子                                                       */
/* ------------------------------------------------------------------ */

/*
 * 在 WebSocket 命令运行期间被注册为 shell_set_print_hook。
 * 将控制台打印的每个字符串转发到所有已连接的浏览器。
 */
static void shell_output_hook(const char *text)
{
    ws_send_raw_to_all(text, strlen(text));
}

/* ------------------------------------------------------------------ */
/* Shell 命令执行任务                                                   */
/* ------------------------------------------------------------------ */

static void ws_shell_task(void *arg)
{
    ws_cmd_item_t item;

    (void)arg;

    for (;;) {
        if (xQueueReceive(s_cmd_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        shell_set_print_hook(shell_output_hook);
        int rc = shell_execute_line(item.line);
        shell_set_print_hook(NULL);

        if (rc > 0) {
            char rc_buf[24];
            int n = snprintf(rc_buf, sizeof(rc_buf), "[rc=%d]\r\n", rc);
            if (n > 0) {
                ws_send_raw_to_all(rc_buf, (size_t)n);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                             */
/* ------------------------------------------------------------------ */

void websocket_shell_init(void)
{
    memset(s_client_fds, -1, sizeof(s_client_fds));
    s_client_count = 0;

    if (s_cmd_queue == NULL) {
        s_cmd_queue = xQueueCreate(WS_CMD_QUEUE_LEN, sizeof(ws_cmd_item_t));
        if (s_cmd_queue == NULL) {
            ESP_LOGE(TAG, "failed to create WS command queue");
            return;
        }
    }

    if (s_shell_task == NULL) {
        BaseType_t ok = xTaskCreate(
            ws_shell_task,
            "ws_shell",
            WS_SHELL_STACK,
            NULL,
            WS_SHELL_PRIORITY,
            &s_shell_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create WS shell task");
            s_shell_task = NULL;
            return;
        }
    }

    ESP_LOGI(TAG, "WebSocket shell bridge ready (max %d clients)", MAX_WS_CLIENTS);
}

void websocket_shell_receive_text(const char *data, size_t len)
{
    ws_cmd_item_t item;

    if (len == 0) {
        return;
    }
    if (len >= WS_CMD_MAX_LEN) {
        ESP_LOGW(TAG, "WS frame too large (%u bytes), ignoring", (unsigned)len);
        ws_send_raw_to_all("[ERR] Command too long\r\n", 24);
        return;
    }
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "WS command queue is not initialized");
        ws_send_raw_to_all("[ERR] shell queue unavailable\r\n", 31);
        return;
    }

    /* 复制为以 NUL 结尾的缓冲区。 */
    memset(&item, 0, sizeof(item));
    memcpy(item.line, data, len);
    item.line[len] = '\0';

    ESP_LOGI(TAG, "WS cmd: %s", item.line);

    /* 将 "pwos> <cmd>" 回显回浏览器，以便 xterm 可以显示提示符。 */
    char echo_buf[WS_CMD_MAX_LEN + 16];
    int  echo_len = snprintf(echo_buf, sizeof(echo_buf), "pwos> %s\r\n", item.line);
    if (echo_len > 0) {
        ws_send_raw_to_all(echo_buf, (size_t)echo_len);
    }

    if (xQueueSend(s_cmd_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "WS command queue full");
        ws_send_raw_to_all("[ERR] shell busy\r\n", 18);
    }
}

void websocket_shell_broadcast(const char *fmt, ...)
{
    char    buf[WS_BROADCAST_CAP];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        ws_send_raw_to_all(buf, len);
    }
}
