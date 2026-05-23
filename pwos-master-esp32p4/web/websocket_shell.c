#include "websocket_shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "shell.h"

#define WS_BROADCAST_CAP 1024
#define WS_CMD_MAX_LEN   256
#define MAX_WS_CLIENTS   4

static const char *TAG = "web_shell";

/* Server handle passed in after httpd_start(); needed for httpd_queue_work. */
static httpd_handle_t s_server = NULL;

/* Active WebSocket client socket FDs (protected by spinlock). */
static int           s_client_fds[MAX_WS_CLIENTS];
static int           s_client_count = 0;
static portMUX_TYPE  s_client_lock  = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/* Client bookkeeping                                                   */
/* ------------------------------------------------------------------ */

void websocket_shell_set_server(httpd_handle_t hd)
{
    s_server = hd;
}

void websocket_shell_client_connected(int fd)
{
    taskENTER_CRITICAL(&s_client_lock);
    if (s_client_count < MAX_WS_CLIENTS) {
        s_client_fds[s_client_count++] = fd;
        ESP_LOGI(TAG, "WS client connected fd=%d (total=%d)", fd, s_client_count);
    } else {
        ESP_LOGW(TAG, "WS client table full, rejecting fd=%d", fd);
    }
    taskEXIT_CRITICAL(&s_client_lock);
}

void websocket_shell_client_disconnected(int fd)
{
    taskENTER_CRITICAL(&s_client_lock);
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) {
            /* Swap-remove for O(1) deletion without preserving order. */
            s_client_fds[i] = s_client_fds[--s_client_count];
            ESP_LOGI(TAG, "WS client disconnected fd=%d (total=%d)", fd, s_client_count);
            break;
        }
    }
    taskEXIT_CRITICAL(&s_client_lock);
}

/* ------------------------------------------------------------------ */
/* Async send machinery                                                 */
/* ------------------------------------------------------------------ */

/*
 * Work item heap-allocated per send per client.
 * httpd_queue_work runs the callback in the httpd task, from which
 * httpd_ws_send_frame_async is safe to call.
 */
typedef struct {
    int    fd;
    size_t len;
    uint8_t data[]; /* flexible array: payload follows struct */
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

/* Send raw bytes to every connected client via httpd_queue_work. */
static void ws_send_raw_to_all(const char *data, size_t len)
{
    if (s_server == NULL || len == 0) {
        return;
    }

    /* Snapshot client list while holding the spinlock. */
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
/* Shell output hook                                                    */
/* ------------------------------------------------------------------ */

/*
 * Registered as shell_set_print_hook while a WebSocket command runs.
 * Forwards each printed string to all connected browsers.
 */
static void shell_output_hook(const char *text)
{
    ws_send_raw_to_all(text, strlen(text));
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void websocket_shell_init(void)
{
    memset(s_client_fds, -1, sizeof(s_client_fds));
    s_client_count = 0;
    ESP_LOGI(TAG, "WebSocket shell bridge ready (max %d clients)", MAX_WS_CLIENTS);
}

void websocket_shell_receive_text(const char *data, size_t len)
{
    if (len == 0) {
        return;
    }
    if (len >= WS_CMD_MAX_LEN) {
        ESP_LOGW(TAG, "WS frame too large (%u bytes), ignoring", (unsigned)len);
        ws_send_raw_to_all("[ERR] Command too long\r\n", 24);
        return;
    }

    /* Copy to NUL-terminated buffer. */
    char line[WS_CMD_MAX_LEN];
    memcpy(line, data, len);
    line[len] = '\0';

    ESP_LOGI(TAG, "WS cmd: %s", line);

    /* Echo "pwos> <cmd>" back to the browser so xterm shows a prompt. */
    char echo_buf[WS_CMD_MAX_LEN + 16];
    int  echo_len = snprintf(echo_buf, sizeof(echo_buf), "pwos> %s\r\n", line);
    if (echo_len > 0) {
        ws_send_raw_to_all(echo_buf, (size_t)echo_len);
    }

    /* Hook shell output so printf/puts inside shell_execute_line reach WS. */
    shell_set_print_hook(shell_output_hook);
    int rc = shell_execute_line(line);
    shell_set_print_hook(NULL);

    /* For non-zero, non-(-1) return codes surface the error to the browser. */
    if (rc > 0) {
        char rc_buf[24];
        int  n = snprintf(rc_buf, sizeof(rc_buf), "[rc=%d]\r\n", rc);
        if (n > 0) {
            ws_send_raw_to_all(rc_buf, (size_t)n);
        }
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
