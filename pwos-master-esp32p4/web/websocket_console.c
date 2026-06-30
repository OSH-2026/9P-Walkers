#include "websocket_console.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PWOS_WS_MAX_CLIENTS 4u
#define PWOS_WS_COMMAND_QUEUE_LEN 8u
#define PWOS_WS_OUTPUT_CAP 2048u
#define PWOS_WS_WORKER_STACK 6144u
#define PWOS_WS_WORKER_PRIO 5u

typedef struct {
    uint8_t used;
    int socket_fd;
    uint32_t generation;
} pwos_ws_client_t;

typedef struct {
    int socket_fd;
    uint32_t generation;
    char line[PWOS_COMMAND_MAX_LINE];
} pwos_ws_command_t;

typedef struct {
    int socket_fd;
    uint32_t generation;
    size_t output_len;
    char output[PWOS_WS_OUTPUT_CAP];
} pwos_ws_result_t;

typedef struct {
    SemaphoreHandle_t mutex;
    QueueHandle_t command_queue;
    TaskHandle_t worker;
    httpd_handle_t server;
    pwos_command_service_t command_service;
    pwos_ws_client_t clients[PWOS_WS_MAX_CLIENTS];
    uint32_t next_generation;
    pwos_websocket_console_status_t status;
} pwos_websocket_console_t;

static const char *TAG = "pwos_ws";
static pwos_websocket_console_t g_console;

static void console_lock(void)
{
    (void)xSemaphoreTake(g_console.mutex, portMAX_DELAY);
}

static void console_unlock(void)
{
    (void)xSemaphoreGive(g_console.mutex);
}

static int find_client_locked(int socket_fd)
{
    size_t i;

    for (i = 0u; i < PWOS_WS_MAX_CLIENTS; ++i) {
        if (g_console.clients[i].used != 0u &&
            g_console.clients[i].socket_fd == socket_fd) {
            return (int)i;
        }
    }
    return -1;
}

static int add_client(int socket_fd, uint32_t *out_generation)
{
    int index;
    size_t i;

    console_lock();
    index = find_client_locked(socket_fd);
    if (index < 0) {
        for (i = 0u; i < PWOS_WS_MAX_CLIENTS; ++i) {
            if (g_console.clients[i].used == 0u) {
                index = (int)i;
                break;
            }
        }
    }
    if (index < 0) {
        console_unlock();
        return -1;
    }

    if (g_console.clients[index].used == 0u) {
        ++g_console.status.clients;
        ++g_console.status.connected;
    }
    ++g_console.next_generation;
    if (g_console.next_generation == 0u) {
        ++g_console.next_generation;
    }
    g_console.clients[index].used = 1u;
    g_console.clients[index].socket_fd = socket_fd;
    g_console.clients[index].generation = g_console.next_generation;
    *out_generation = g_console.next_generation;
    console_unlock();
    return 0;
}

static int get_client_generation(int socket_fd, uint32_t *out_generation)
{
    int index;

    console_lock();
    index = find_client_locked(socket_fd);
    if (index >= 0) {
        *out_generation = g_console.clients[index].generation;
    }
    console_unlock();
    return index >= 0 ? 0 : -1;
}

static int client_is_current(int socket_fd, uint32_t generation)
{
    int index;
    int current = 0;

    console_lock();
    index = find_client_locked(socket_fd);
    if (index >= 0 && g_console.clients[index].generation == generation) {
        current = 1;
    }
    console_unlock();
    return current;
}

static void send_result_work(void *arg)
{
    pwos_ws_result_t *result = (pwos_ws_result_t *)arg;
    httpd_ws_frame_t frame;
    httpd_handle_t server;
    esp_err_t error;

    if (result == NULL) {
        return;
    }

    console_lock();
    server = g_console.server;
    console_unlock();
    if (server == NULL ||
        client_is_current(result->socket_fd, result->generation) == 0 ||
        httpd_ws_get_fd_info(server, result->socket_fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        console_lock();
        ++g_console.status.stale_results;
        console_unlock();
        free(result);
        return;
    }

    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)result->output;
    frame.len = result->output_len;
    error = httpd_ws_send_frame_async(server, result->socket_fd, &frame);
    if (error != ESP_OK) {
        console_lock();
        ++g_console.status.send_failures;
        console_unlock();
    }
    free(result);
}

static void queue_result(pwos_ws_result_t *result)
{
    httpd_handle_t server;
    esp_err_t error;

    console_lock();
    server = g_console.server;
    console_unlock();
    if (server == NULL) {
        free(result);
        return;
    }

    error = httpd_queue_work(server, send_result_work, result);
    if (error != ESP_OK) {
        console_lock();
        ++g_console.status.send_failures;
        console_unlock();
        free(result);
    }
}

static void command_worker(void *arg)
{
    pwos_ws_command_t command;

    (void)arg;
    for (;;) {
        pwos_ws_result_t *result;
        int rc;

        if (xQueueReceive(g_console.command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (client_is_current(command.socket_fd, command.generation) == 0) {
            console_lock();
            ++g_console.status.stale_results;
            console_unlock();
            continue;
        }

        result = (pwos_ws_result_t *)calloc(1u, sizeof(*result));
        if (result == NULL) {
            console_lock();
            ++g_console.status.commands_failed;
            ++g_console.status.send_failures;
            console_unlock();
            continue;
        }
        result->socket_fd = command.socket_fd;
        result->generation = command.generation;
        rc = pwos_command_service_execute(
            &g_console.command_service,
            command.line,
            result->output,
            sizeof(result->output),
            &result->output_len);
        console_lock();
        ++g_console.status.commands_completed;
        if (rc != 0) {
            ++g_console.status.commands_failed;
        }
        console_unlock();
        queue_result(result);
    }
}

static esp_err_t send_queue_full(httpd_req_t *request)
{
    static const char message[] = "error: command queue full (-1003)\r\n";
    httpd_ws_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)message;
    frame.len = sizeof(message) - 1u;
    return httpd_ws_send_frame(request, &frame);
}

int pwos_websocket_console_init(
    const pwos_command_service_config_t *command_config)
{
    BaseType_t created;
    int rc;

    if (command_config == NULL) {
        return -1;
    }
    if (g_console.status.initialized != 0u) {
        return 0;
    }

    memset(&g_console, 0, sizeof(g_console));
    g_console.mutex = xSemaphoreCreateMutex();
    g_console.command_queue = xQueueCreate(
        PWOS_WS_COMMAND_QUEUE_LEN, sizeof(pwos_ws_command_t));
    if (g_console.mutex == NULL || g_console.command_queue == NULL) {
        if (g_console.command_queue != NULL) {
            vQueueDelete(g_console.command_queue);
        }
        if (g_console.mutex != NULL) {
            vSemaphoreDelete(g_console.mutex);
        }
        memset(&g_console, 0, sizeof(g_console));
        return -1;
    }

    rc = pwos_command_service_init(&g_console.command_service, command_config);
    if (rc != 0) {
        vQueueDelete(g_console.command_queue);
        vSemaphoreDelete(g_console.mutex);
        memset(&g_console, 0, sizeof(g_console));
        return rc;
    }
    g_console.status.initialized = 1u;

    created = xTaskCreate(
        command_worker,
        "pwos_web_cmd",
        PWOS_WS_WORKER_STACK,
        NULL,
        PWOS_WS_WORKER_PRIO,
        &g_console.worker);
    if (created != pdPASS) {
        vQueueDelete(g_console.command_queue);
        vSemaphoreDelete(g_console.mutex);
        memset(&g_console, 0, sizeof(g_console));
        return -1;
    }
    g_console.status.worker_started = 1u;
    return 0;
}

void pwos_websocket_console_set_server(httpd_handle_t server)
{
    if (g_console.mutex == NULL) {
        return;
    }
    console_lock();
    g_console.server = server;
    console_unlock();
}

esp_err_t pwos_websocket_console_handle(httpd_req_t *request)
{
    pwos_ws_command_t command;
    httpd_ws_frame_t frame;
    int socket_fd;
    esp_err_t error;

    if (request == NULL || g_console.status.initialized == 0u) {
        return ESP_ERR_INVALID_STATE;
    }

    socket_fd = httpd_req_to_sockfd(request);
    if (request->method == HTTP_GET) {
        uint32_t generation;

        if (add_client(socket_fd, &generation) != 0) {
            ESP_LOGW(TAG, "reject fd=%d: client table full", socket_fd);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "client connected fd=%d generation=%lu",
            socket_fd, (unsigned long)generation);
        return ESP_OK;
    }

    memset(&frame, 0, sizeof(frame));
    error = httpd_ws_recv_frame(request, &frame, 0u);
    if (error != ESP_OK) {
        return error;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0u) {
        return ESP_OK;
    }
    if (frame.len >= sizeof(command.line)) {
        static const char too_long[] = "error: command too long\r\n";
        httpd_ws_frame_t response = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)too_long,
            .len = sizeof(too_long) - 1u,
        };
        return httpd_ws_send_frame(request, &response);
    }

    memset(&command, 0, sizeof(command));
    command.socket_fd = socket_fd;
    if (get_client_generation(socket_fd, &command.generation) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    frame.payload = (uint8_t *)command.line;
    error = httpd_ws_recv_frame(request, &frame, frame.len);
    if (error != ESP_OK) {
        return error;
    }
    command.line[frame.len] = '\0';

    console_lock();
    ++g_console.status.commands_received;
    console_unlock();
    if (xQueueSend(g_console.command_queue, &command, 0u) != pdTRUE) {
        esp_err_t send_error;

        console_lock();
        ++g_console.status.queue_full;
        console_unlock();
        send_error = send_queue_full(request);
        if (send_error != ESP_OK) {
            console_lock();
            ++g_console.status.send_failures;
            console_unlock();
        }
    }
    return ESP_OK;
}

void pwos_websocket_console_client_disconnected(int socket_fd)
{
    int index;

    if (g_console.mutex == NULL) {
        return;
    }
    console_lock();
    index = find_client_locked(socket_fd);
    if (index >= 0) {
        memset(&g_console.clients[index], 0, sizeof(g_console.clients[index]));
        if (g_console.status.clients > 0u) {
            --g_console.status.clients;
        }
        ++g_console.status.disconnected;
    }
    console_unlock();
}

void pwos_websocket_console_get_status(
    pwos_websocket_console_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    if (g_console.mutex == NULL) {
        memset(out_status, 0, sizeof(*out_status));
        return;
    }
    console_lock();
    *out_status = g_console.status;
    console_unlock();
}
