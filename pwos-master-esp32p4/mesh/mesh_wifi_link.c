#include "mesh_wifi_link.h"

#include <stdio.h>
#include <string.h>

#include "mesh_protocal.h"

#ifdef ESP_PLATFORM

#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

/* 最大合法帧 = MESH_FRAME_OVERHEAD + MESH_MAX_PAYLOAD_LEN = 526，留余量。 */
#define MESH_WIFI_LINK_RX_BUF_CAP 1024u
#define MESH_WIFI_LINK_LOCK_TIMEOUT_MS 100u
#define MESH_WIFI_LINK_SEND_TIMEOUT_MS 200u

static const char *TAG = "mesh_wifi_link";

struct mesh_wifi_link_state {
    bool active;
    uint16_t tcp_port;
    int listen_fd;
    int client_fd;
    char client_ip[16];

    /* TCP 字节流重组缓冲：帧边界由 'M''H'+FrameLen 自描述。 */
    uint8_t rx_buf[MESH_WIFI_LINK_RX_BUF_CAP];
    size_t rx_have;

    /* 经本链路收到过的 src 地址表（同一 TCP 入口可挂 relay 带多节点）。 */
    uint8_t addrs[MESH_WIFI_LINK_MAX_ADDRS];
    size_t addr_count;
};

static struct mesh_wifi_link_state s_link = {
    .listen_fd = -1,
    .client_fd = -1,
};

/* 单把状态锁：连接管理 + 收发均为非阻塞短临界区，串行化足够。 */
static SemaphoreHandle_t s_lock;

static int link_take_lock(void)
{
    if (s_lock == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(MESH_WIFI_LINK_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return -(int)MESH_ERR_BUSY;
    }
    return 0;
}

static void link_give_lock(void)
{
    xSemaphoreGive(s_lock);
}

static int link_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void link_close_client_locked(void)
{
    if (s_link.client_fd >= 0) {
        close(s_link.client_fd);
        s_link.client_fd = -1;
    }
    s_link.client_ip[0] = '\0';
    s_link.rx_have = 0u;
    /* 链路断开后旧地址不再可达，清表让发送路径回落广播/UART。 */
    s_link.addr_count = 0u;
}

static void link_stop_locked(void)
{
    link_close_client_locked();
    if (s_link.listen_fd >= 0) {
        close(s_link.listen_fd);
        s_link.listen_fd = -1;
    }
    s_link.active = false;
    s_link.tcp_port = 0u;
}

static void link_learn_addr_locked(uint8_t mesh_addr)
{
    size_t i;

    if (mesh_addr == MESH_ADDR_UNASSIGNED || mesh_addr == MESH_ADDR_HOST) {
        return;
    }
    for (i = 0u; i < s_link.addr_count; ++i) {
        if (s_link.addrs[i] == mesh_addr) {
            return;
        }
    }
    if (s_link.addr_count < MESH_WIFI_LINK_MAX_ADDRS) {
        s_link.addrs[s_link.addr_count++] = mesh_addr;
        ESP_LOGI(TAG, "learned mesh addr 0x%02x via LAN TCP link", mesh_addr);
    }
}

/* 非阻塞 accept：新连接替换旧连接（透传模块重连是常态）。 */
static void link_poll_accept_locked(void)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd;

    if (s_link.listen_fd < 0) {
        return;
    }

    fd = accept(s_link.listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
        return;
    }

    if (link_set_nonblocking(fd) != 0) {
        close(fd);
        return;
    }

    {
        int nodelay = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    if (s_link.client_fd >= 0) {
        ESP_LOGW(TAG, "new client replaces existing connection");
        link_close_client_locked();
    }

    s_link.client_fd = fd;
    inet_ntoa_r(peer.sin_addr, s_link.client_ip, sizeof(s_link.client_ip));
    ESP_LOGI(TAG, "client connected from %s", s_link.client_ip);
}

static void link_drop_rx_bytes_locked(size_t n)
{
    if (n >= s_link.rx_have) {
        s_link.rx_have = 0u;
        return;
    }
    memmove(s_link.rx_buf, s_link.rx_buf + n, s_link.rx_have - n);
    s_link.rx_have -= n;
}

/*
 * 从重组缓冲中提取一个完整帧。失步时丢弃字节重新对齐到 'M''H' 魔数，
 * 与 UART 路径不同：TCP 不丢字节，失步只可能来自对端发了非帧数据。
 */
static int link_extract_frame_locked(uint8_t *rx_data, size_t rx_cap, size_t *rx_len)
{
    for (;;) {
        uint16_t frame_len_field;
        size_t total_len;

        /* 重对齐到下一个可能的魔数起点。 */
        if (s_link.rx_have >= 1u && s_link.rx_buf[0] != (uint8_t)'M') {
            const uint8_t *magic = memchr(s_link.rx_buf, 'M', s_link.rx_have);

            if (magic == NULL) {
                s_link.rx_have = 0u;
                return -(int)MESH_ERR_BUSY;
            }
            link_drop_rx_bytes_locked((size_t)(magic - s_link.rx_buf));
        }
        if (s_link.rx_have < 4u) {
            return -(int)MESH_ERR_BUSY;
        }
        if (s_link.rx_buf[1] != (uint8_t)'H') {
            link_drop_rx_bytes_locked(1u);
            continue;
        }

        frame_len_field = (uint16_t)s_link.rx_buf[2] | (uint16_t)((uint16_t)s_link.rx_buf[3] << 8);
        total_len = (size_t)frame_len_field + 6u;
        if (frame_len_field < 8u || total_len > MESH_WIFI_LINK_RX_BUF_CAP) {
            /* 长度非法：当前魔数是误对齐，跳过它继续找。 */
            link_drop_rx_bytes_locked(2u);
            continue;
        }
        if (s_link.rx_have < total_len) {
            return -(int)MESH_ERR_BUSY;
        }
        if (total_len > rx_cap) {
            link_drop_rx_bytes_locked(total_len);
            return -(int)MESH_ERR_BAD_FRAME;
        }

        memcpy(rx_data, s_link.rx_buf, total_len);
        *rx_len = total_len;

        /* Src 位于固定偏移 6（Magic2+FrameLen2+Version1+Type1）。 */
        link_learn_addr_locked(s_link.rx_buf[6]);

        link_drop_rx_bytes_locked(total_len);
        return 0;
    }
}

int mesh_wifi_link_start(uint16_t tcp_port)
{
    struct sockaddr_in addr;
    int fd;
    int rc;

    if (tcp_port == 0u) {
        tcp_port = (uint16_t)MESH_WIFI_LINK_DEFAULT_TCP_PORT;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return -(int)MESH_ERR_BUSY;
        }
    }

    rc = link_take_lock();
    if (rc != 0) {
        return rc;
    }

    if (s_link.active && s_link.tcp_port == tcp_port) {
        link_give_lock();
        return 0;
    }
    if (s_link.active) {
        link_stop_locked();
    }

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        link_give_lock();
        return -(int)MESH_ERR_INVALID_STATE;
    }

    {
        int reuse = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(tcp_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0 ||
        link_set_nonblocking(fd) != 0) {
        close(fd);
        link_give_lock();
        return -(int)MESH_ERR_INVALID_STATE;
    }

    s_link.listen_fd = fd;
    s_link.tcp_port = tcp_port;
    s_link.rx_have = 0u;
    s_link.addr_count = 0u;
    s_link.active = true;
    link_give_lock();

    ESP_LOGI(TAG, "listening on tcp port %u", (unsigned)tcp_port);
    return 0;
}

int mesh_wifi_link_stop(void)
{
    int rc;

    if (s_lock == NULL) {
        return 0;
    }

    rc = link_take_lock();
    if (rc != 0) {
        return rc;
    }
    link_stop_locked();
    link_give_lock();

    ESP_LOGI(TAG, "stopped");
    return 0;
}

bool mesh_wifi_link_active(void)
{
    return s_link.active;
}

bool mesh_wifi_link_owns_addr(uint8_t mesh_addr)
{
    size_t i;
    bool found = false;

    if (!s_link.active || link_take_lock() != 0) {
        return false;
    }
    for (i = 0u; i < s_link.addr_count; ++i) {
        if (s_link.addrs[i] == mesh_addr) {
            found = true;
            break;
        }
    }
    link_give_lock();
    return found;
}

int mesh_wifi_link_send_frame(const uint8_t *tx_data, size_t tx_len)
{
    size_t total = 0u;
    TickType_t deadline;
    int rc;

    if (tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    rc = link_take_lock();
    if (rc != 0) {
        return rc;
    }
    if (!s_link.active) {
        link_give_lock();
        return -(int)MESH_ERR_INVALID_STATE;
    }
    if (s_link.client_fd < 0) {
        /* 顺手驱动一次 accept，缩短刚连入 client 的首帧延迟。 */
        link_poll_accept_locked();
    }
    if (s_link.client_fd < 0) {
        link_give_lock();
        return -(int)MESH_ERR_NO_ROUTE;
    }

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(MESH_WIFI_LINK_SEND_TIMEOUT_MS);
    while (total < tx_len) {
        ssize_t written = send(s_link.client_fd, tx_data + total, tx_len - total, 0);

        if (written > 0) {
            total += (size_t)written;
            continue;
        }
        if (written < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            if (xTaskGetTickCount() >= deadline) {
                link_give_lock();
                return -(int)MESH_ERR_BUSY;
            }
            vTaskDelay(1);
            continue;
        }

        ESP_LOGW(TAG, "send failed (errno=%d), dropping client", errno);
        link_close_client_locked();
        link_give_lock();
        return -(int)MESH_ERR_NO_ROUTE;
    }

    link_give_lock();
    return 0;
}

int mesh_wifi_link_receive_frame(uint8_t *rx_data, size_t rx_cap, size_t *rx_len)
{
    int rc;

    if (rx_data == NULL || rx_len == NULL || rx_cap < MESH_FRAME_OVERHEAD) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    *rx_len = 0u;

    if (!s_link.active) {
        return -(int)MESH_ERR_BUSY;
    }

    rc = link_take_lock();
    if (rc != 0) {
        return rc;
    }
    if (!s_link.active) {
        link_give_lock();
        return -(int)MESH_ERR_BUSY;
    }

    link_poll_accept_locked();

    if (s_link.client_fd >= 0 && s_link.rx_have < MESH_WIFI_LINK_RX_BUF_CAP) {
        ssize_t got = recv(
            s_link.client_fd,
            s_link.rx_buf + s_link.rx_have,
            MESH_WIFI_LINK_RX_BUF_CAP - s_link.rx_have,
            0);

        if (got > 0) {
            s_link.rx_have += (size_t)got;
        } else if (got == 0) {
            ESP_LOGI(TAG, "client %s disconnected", s_link.client_ip);
            link_close_client_locked();
        } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
            ESP_LOGW(TAG, "recv failed (errno=%d), dropping client", errno);
            link_close_client_locked();
        }
    }

    rc = link_extract_frame_locked(rx_data, rx_cap, rx_len);
    link_give_lock();
    return rc;
}

int mesh_wifi_link_format_status(char *out, size_t out_cap)
{
    int written;
    int rc;

    if (out == NULL || out_cap == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    if (!s_link.active) {
        written = snprintf(out, out_cap, "lan tcp link: inactive (use: wifi start [port])\n");
        return written < 0 ? -(int)MESH_ERR_INVALID_STATE : written;
    }

    rc = link_take_lock();
    if (rc != 0) {
        return rc;
    }

    written = snprintf(
        out,
        out_cap,
        "lan tcp link: listening on tcp/%u\nclient      : %s\n",
        (unsigned)s_link.tcp_port,
        s_link.client_fd >= 0 ? s_link.client_ip : "(none)");
    if (written > 0 && (size_t)written < out_cap) {
        int n = snprintf(out + written, out_cap - (size_t)written, "learned  :");

        if (n > 0) {
            written += n;
            for (size_t i = 0u; i < s_link.addr_count && (size_t)written < out_cap; ++i) {
                n = snprintf(out + written, out_cap - (size_t)written, " 0x%02x", s_link.addrs[i]);
                if (n <= 0) {
                    break;
                }
                written += n;
            }
            if ((size_t)written < out_cap) {
                n = snprintf(out + written, out_cap - (size_t)written,
                             s_link.addr_count == 0u ? " (none)\n" : "\n");
                if (n > 0) {
                    written += n;
                }
            }
        }
    }

    link_give_lock();
    return written < 0 ? -(int)MESH_ERR_INVALID_STATE : written;
}

#else /* !ESP_PLATFORM：PC host 测试用空实现，链路恒为不可用。 */

int mesh_wifi_link_start(uint16_t tcp_port)
{
    (void)tcp_port;
    return -(int)MESH_ERR_INVALID_STATE;
}

int mesh_wifi_link_stop(void)
{
    return 0;
}

bool mesh_wifi_link_active(void)
{
    return false;
}

bool mesh_wifi_link_owns_addr(uint8_t mesh_addr)
{
    (void)mesh_addr;
    return false;
}

int mesh_wifi_link_send_frame(const uint8_t *tx_data, size_t tx_len)
{
    (void)tx_data;
    (void)tx_len;
    return -(int)MESH_ERR_NO_ROUTE;
}

int mesh_wifi_link_receive_frame(uint8_t *rx_data, size_t rx_cap, size_t *rx_len)
{
    (void)rx_data;
    (void)rx_cap;
    if (rx_len != NULL) {
        *rx_len = 0u;
    }
    return -(int)MESH_ERR_BUSY;
}

int mesh_wifi_link_format_status(char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }
    return snprintf(out, out_cap, "lan tcp link: not supported on this platform\n");
}

#endif /* ESP_PLATFORM */
