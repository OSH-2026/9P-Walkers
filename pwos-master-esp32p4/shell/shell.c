#include "shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_port.h"
#include "mini9p_client.h"
#include "mini9p_protocol.h"

#define SHELL_TASK_STACK_SIZE 4096
#define SHELL_TASK_PRIORITY 5
#define SHELL_LINE_CAP 256
#define SHELL_PRINT_CAP 320
#define SHELL_UART_PORT UART_NUM_1
#define SHELL_UART_DEFAULT_TX_PIN 17
#define SHELL_UART_DEFAULT_RX_PIN 18
#define SHELL_UART_DEFAULT_BAUD 1000000
#define SHELL_UART_DEFAULT_TIMEOUT_MS 50u
#define SHELL_UART_DRIVER_BUFFER 1024

enum shell_transport_kind {
    SHELL_TRANSPORT_UART = 0,
    SHELL_TRANSPORT_MOCK,
};

struct shell_uart_transport {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    uint32_t timeout_ms;
    bool initialized;
};

static struct m9p_client shell_m9p_client;
static bool shell_m9p_initialized = false;
static enum shell_transport_kind shell_transport_kind = SHELL_TRANSPORT_UART;
static struct shell_uart_transport shell_uart_transport = {
    .port = SHELL_UART_PORT,
    .tx_pin = SHELL_UART_DEFAULT_TX_PIN,
    .rx_pin = SHELL_UART_DEFAULT_RX_PIN,
    .baud_rate = SHELL_UART_DEFAULT_BAUD,
    .timeout_ms = SHELL_UART_DEFAULT_TIMEOUT_MS,
    .initialized = false,
};
static shell_output_fn shell_output_hook = NULL;
static void *shell_output_hook_ctx = NULL;

static void shell_emit_text(const char *text)
{
    if (text == NULL) {
        return;
    }

    fputs(text, stdout);
    if (shell_output_hook != NULL) {
        shell_output_hook(shell_output_hook_ctx, text);
    }
}

static void shell_printf(const char *fmt, ...)
{
    char buffer[SHELL_PRINT_CAP];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (len <= 0) {
        return;
    }

    shell_emit_text(buffer);
}

static void shell_puts(const char *text)
{
    if (text != NULL) {
        shell_emit_text(text);
    }
    shell_emit_text("\n");
}

void shell_set_output_hook(shell_output_fn hook, void *ctx)
{
    shell_output_hook = hook;
    shell_output_hook_ctx = ctx;
}

/**
 * @brief 模拟的传输层回调函数 (Transport Callback)
 * 这个函数仅用于测试环境。它拦截所有准备发往底层的 mini9p 数据包，并将其大小打印出来。
 * 通过在本地伪造空的返回响应，使得在没有接上实体从机的情况下，上层逻辑也不至于阻塞。
 */
static int mock_transport(void *transport_ctx, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data, size_t rx_cap, size_t *rx_len)
{
    (void)transport_ctx;
    (void)tx_data;
    (void)rx_data;
    (void)rx_cap;
    shell_printf("[mock m9p] tx=%u bytes\n", (unsigned)tx_len);
    *rx_len = 0; /* 伪造空的返回数据 */
    return -(int)M9P_ERR_EIO;
}

/**
 * @brief 释放并重建 m9p 客户端实例，使 transport 切换后状态一致。
 */
static void reset_m9p_client(void)
{
    memset(&shell_m9p_client, 0, sizeof(shell_m9p_client));
    shell_m9p_initialized = false;
}

static void resync_serial_frame(uint8_t *buffer, size_t *len)
{
    size_t offset;

    for (offset = 1u; offset + 1u < *len; ++offset) {
        if (buffer[offset] == (uint8_t)'9' && buffer[offset + 1u] == (uint8_t)'P') {
            memmove(buffer, buffer + offset, *len - offset);
            *len -= offset;
            return;
        }
    }

    if (*len > 0u && buffer[*len - 1u] == (uint8_t)'9') {
        buffer[0] = (uint8_t)'9';
        *len = 1u;
        return;
    }

    *len = 0u;
}

static bool ensure_uart_transport_ready(void)
{
    const uart_config_t config = {
        .baud_rate = shell_uart_transport.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (shell_uart_transport.initialized) {
        return true;
    }
    if (uart_driver_install(shell_uart_transport.port, SHELL_UART_DRIVER_BUFFER, 0, 0, NULL, 0) != ESP_OK) {
        return false;
    }
    if (uart_param_config(shell_uart_transport.port, &config) != ESP_OK) {
        (void)uart_driver_delete(shell_uart_transport.port);
        return false;
    }
    if (uart_set_pin(
            shell_uart_transport.port,
            shell_uart_transport.tx_pin,
            shell_uart_transport.rx_pin,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE) != ESP_OK) {
        (void)uart_driver_delete(shell_uart_transport.port);
        return false;
    }

    shell_uart_transport.initialized = true;
    return true;
}

static int uart_transport(
    void *transport_ctx,
    const uint8_t *tx_data,
    size_t tx_len,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    struct shell_uart_transport *transport = (struct shell_uart_transport *)transport_ctx;
    int written;
    int64_t deadline_us;
    size_t received = 0u;

    if (transport == NULL || tx_data == NULL || rx_data == NULL || rx_len == NULL) {
        return -(int)M9P_ERR_EINVAL;
    }
    if (!ensure_uart_transport_ready()) {
        return -(int)M9P_ERR_EIO;
    }

    *rx_len = 0u;
    (void)uart_flush_input(transport->port);
    written = uart_write_bytes(transport->port, tx_data, tx_len);
    if (written < 0 || (size_t)written != tx_len) {
        return -(int)M9P_ERR_EIO;
    }
    if (uart_wait_tx_done(transport->port, pdMS_TO_TICKS(transport->timeout_ms)) != ESP_OK) {
        return -(int)M9P_ERR_EIO;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)transport->timeout_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        uint8_t byte = 0u;
        int read_len = uart_read_bytes(transport->port, &byte, 1u, pdMS_TO_TICKS(5u));

        if (read_len <= 0) {
            continue;
        }

        if (received == 0u) {
            if (byte == (uint8_t)'9') {
                rx_data[received++] = byte;
            }
            continue;
        }
        if (received == 1u) {
            if (byte == (uint8_t)'P') {
                rx_data[received++] = byte;
            } else {
                rx_data[0] = byte;
                received = byte == (uint8_t)'9' ? 1u : 0u;
            }
            continue;
        }
        if (received >= rx_cap) {
            resync_serial_frame(rx_data, &received);
        }
        if (received >= rx_cap) {
            received = 0u;
        }

        rx_data[received++] = byte;
        if (received >= 4u) {
            uint16_t frame_len_field = (uint16_t)rx_data[2] | (uint16_t)((uint16_t)rx_data[3] << 8);
            size_t expected_len = (size_t)frame_len_field + 6u;

            if (expected_len < M9P_FRAME_OVERHEAD || expected_len > rx_cap) {
                if (expected_len > rx_cap) {
                    return -(int)M9P_ERR_EMSIZE;
                }
                resync_serial_frame(rx_data, &received);
                continue;
            }
            if (received == expected_len) {
                *rx_len = received;
                return 0;
            }
            if (received > expected_len) {
                resync_serial_frame(rx_data, &received);
            }
        }
    }

    return -(int)M9P_ERR_EAGAIN;
}

static int ensure_m9p_client_initialized(void)
{
    if (shell_m9p_initialized) {
        return 0;
    }

    if (shell_transport_kind == SHELL_TRANSPORT_UART) {
        if (!ensure_uart_transport_ready()) {
            return -(int)M9P_ERR_EIO;
        }
        m9p_client_init(&shell_m9p_client, uart_transport, &shell_uart_transport);
    } else {
        m9p_client_init(&shell_m9p_client, mock_transport, NULL);
    }
    shell_m9p_initialized = true;
    return 0;
}

static int ensure_m9p_session(void)
{
    int rc = ensure_m9p_client_initialized();

    if (rc != 0) {
        return rc;
    }
    if (shell_m9p_client.attached) {
        return 0;
    }

    rc = m9p_client_attach(&shell_m9p_client, M9P_CLIENT_BUFFER_CAP, 1u, 0u);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/**
 * @brief 在原地解析命令语句的小型辅助函数。
 * 我们把解析工作写在这个文件里，保证未来外壳功能更容易拆分或替换。
 * 当前只跳过最基本的前导空格以及制表符。
 */
static char *skip_spaces(char *text)
{
    while (*text == ' ' || *text == '\t') {
        ++text;
    }

    return text;
}

/**
 * @brief 在原地裁切当前命令末尾的所有无效换行与留白。
 */
static void trim_line(char *line)
{
    size_t len = strlen(line);

    while (len > 0u) {
        char ch = line[len - 1u];

        if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') {
            break;
        }
        line[len - 1u] = '\0';
        --len;
    }
}

/**
 * @brief 在终端打印可用命令的内置指引菜单。
 */
static void print_banner(void)
{
    shell_puts("");
    shell_puts("pwos shell");
    shell_puts("  help                 显示命令菜单(show commands)");
    shell_puts("  heap                 打印可用堆内存信息(print free heap)");
    shell_puts("  echo ...             回显输入文本(print text)");
    shell_puts("  ls <dir>             枚举远端目录(list remote directory)");
    shell_puts("  cat <file>           读取远端文本文件(read remote file)");
    shell_puts("  write <path> <text>  写入远端节点(write remote file)");
    shell_puts("  stat <path>          打印远端节点元信息(print remote stat)");
    shell_puts("  lua                  执行内置测试用Lua剧本(run built-in Lua demo)");
    shell_puts("  lua ...              执行随后输入的一行Lua语句(execute inline Lua chunk)");
    shell_puts("  m9p_attach           测试向从机发起 attach(test mini9p attach)");
    shell_puts("  m9p_walk <path>      测试路径 walk(test mini9p walk)");
    shell_puts("  set_transport ...    切换 transport(set uart/mock)");
    shell_puts("  m9p N                查阅 mini9p 状态码名称(print mini9p error name)");
    shell_puts("  reboot               系统重启(restart the board)");
    shell_puts("  支持 /mcu1/... 映射到当前单从机节点");
    shell_puts("");
}

static void print_m9p_result(const char *op, int rc)
{
    if (rc == 0) {
        shell_printf("%s: ok\n", op);
        return;
    }

    shell_printf("%s: rc=%d (%s)\n", op, rc, m9p_error_name((uint16_t)(rc < 0 ? -rc : rc)));
}

static const char *normalize_remote_path(const char *path, char *buffer, size_t buffer_cap)
{
    const char *normalized = path;

    if (path == NULL || buffer == NULL || buffer_cap == 0u) {
        return path;
    }

    if (strncmp(path, "/mcu1", 5u) == 0 && (path[5] == '\0' || path[5] == '/')) {
        normalized = path[5] == '\0' ? "/" : path + 5;
    } else if (strncmp(path, "mcu1/", 5u) == 0) {
        normalized = path + 4;
    }

    snprintf(buffer, buffer_cap, "%s", normalized);
    return buffer;
}

static char *next_token(char **cursor)
{
    char *token = *cursor;

    if (token == NULL) {
        return NULL;
    }

    token = skip_spaces(token);
    if (*token == '\0') {
        *cursor = token;
        return NULL;
    }

    *cursor = token;
    while (**cursor != '\0' && **cursor != ' ' && **cursor != '\t') {
        ++(*cursor);
    }
    if (**cursor != '\0') {
        *(*cursor)++ = '\0';
    }

    return token;
}

static void print_qid(const struct m9p_qid *qid)
{
    if (qid == NULL) {
        return;
    }

    shell_printf(
        "qid: type=0x%02x version=%u object=%lu\n",
        (unsigned)qid->type,
        (unsigned)qid->version,
        (unsigned long)qid->object_id);
}

/**
 * @brief 处理 lua 代码或者交互脚本
 * 如果直接输入 "lua"，则执行系统内置 C 结构中的 demo 数据；
 * 否则的话如果输入形如 "lua print(1)" ，则直接运行解释器执行字符串。
 */
static int handle_lua(const char *args)
{
    /* Bare "lua" runs the canned demo; "lua <chunk>" executes inline code. */
    if (args == NULL || *args == '\0') {
        return pw_lua_run_demo();
    }

    return pw_lua_run_buffer("shell", args);
}

/**
 * @brief 处理 m9p [code] 命令，方便根据代码回溯文字。
 */
static int handle_m9p(const char *args)
{
    char *end = NULL;
    long code;

    if (args == NULL || *args == '\0') {
        shell_puts("usage: m9p <code>");
        return -1;
    }

    code = strtol(args, &end, 0);
    if (end == args) {
        shell_puts("invalid code");
        return -1;
    }

    shell_printf("m9p[%ld] = %s\n", code, m9p_error_name((uint16_t)code));
    return 0;
}

/**
 * @brief 输出(echo)命令处理器
 * 将用户敲入的文本原样输出，常用于测试 shell 解析行为是否正确包含空格。
 */
static int handle_echo(const char *args)
{
    if (args == NULL) {
        shell_puts("");
        return 0;
    }
    shell_puts(args);
    return 0;
}

static int handle_set_transport(const char *args)
{
    char buffer[SHELL_LINE_CAP];
    char *cursor;
    char *kind;
    char *baud_text;
    char *tx_text;
    char *rx_text;

    if (args == NULL || *args == '\0') {
        shell_puts("usage: set_transport <uart [baud [tx rx]]|mock|spi|wifi>");
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", args);
    cursor = buffer;
    kind = next_token(&cursor);
    if (kind == NULL) {
        shell_puts("usage: set_transport <uart [baud [tx rx]]|mock|spi|wifi>");
        return -1;
    }
    if (strcmp(kind, "mock") == 0) {
        shell_transport_kind = SHELL_TRANSPORT_MOCK;
        reset_m9p_client();
        shell_puts("transport set to mock");
        return 0;
    }
    if (strcmp(kind, "uart") == 0) {
        long baud_rate = shell_uart_transport.baud_rate;
        long tx_pin = shell_uart_transport.tx_pin;
        long rx_pin = shell_uart_transport.rx_pin;

        baud_text = next_token(&cursor);
        tx_text = next_token(&cursor);
        rx_text = next_token(&cursor);
        if (baud_text != NULL) {
            baud_rate = strtol(baud_text, NULL, 10);
        }
        if (tx_text != NULL) {
            tx_pin = strtol(tx_text, NULL, 10);
        }
        if (rx_text != NULL) {
            rx_pin = strtol(rx_text, NULL, 10);
        }
        if (baud_rate <= 0 || tx_pin < 0 || rx_pin < 0) {
            shell_puts("invalid uart settings");
            return -1;
        }

        if (shell_uart_transport.initialized) {
            (void)uart_driver_delete(shell_uart_transport.port);
            shell_uart_transport.initialized = false;
        }
        shell_transport_kind = SHELL_TRANSPORT_UART;
        shell_uart_transport.baud_rate = (int)baud_rate;
        shell_uart_transport.tx_pin = (int)tx_pin;
        shell_uart_transport.rx_pin = (int)rx_pin;
        reset_m9p_client();
        shell_printf(
            "transport set to uart baud=%d tx=%d rx=%d\n",
            shell_uart_transport.baud_rate,
            shell_uart_transport.tx_pin,
            shell_uart_transport.rx_pin);
        return 0;
    }
    if (strcmp(kind, "spi") == 0 || strcmp(kind, "wifi") == 0) {
        shell_printf("transport %s not implemented yet\n", kind);
        return -(int)M9P_ERR_ENOTSUP;
    }

    shell_puts("unknown transport");
    return -1;
}

/**
 * @brief 列表(ls)命令处理器示例
 * 读取目录节点并解析远端返回的 dirent 列表。
 */
static int handle_ls(const char *args)
{
    char normalized_path[SHELL_LINE_CAP];
    struct m9p_open_result open_result;
    struct m9p_dirent entries[16];
    uint8_t raw[256];
    uint16_t fid;
    uint16_t count = sizeof(raw);
    size_t entry_count;
    int rc;
    size_t idx;

    if (args == NULL || *args == '\0') {
        shell_puts("usage: ls <path>");
        return -1;
    }

    rc = ensure_m9p_session();
    if (rc != 0) {
        print_m9p_result("attach", rc);
        return rc;
    }

    rc = m9p_client_open_path(
        &shell_m9p_client,
        normalize_remote_path(args, normalized_path, sizeof(normalized_path)),
        M9P_OREAD,
        &fid,
        &open_result);
    if (rc != 0) {
        print_m9p_result("ls open", rc);
        return rc;
    }

    rc = m9p_client_read(&shell_m9p_client, fid, 0u, raw, &count);
    if (rc != 0) {
        (void)m9p_client_clunk(&shell_m9p_client, fid);
        print_m9p_result("ls read", rc);
        return rc;
    }

    entry_count = m9p_parse_dirents(raw, count, entries, sizeof(entries) / sizeof(entries[0]));
    if (entry_count == 0u) {
        shell_puts("(empty or not a directory)");
    }
    for (idx = 0u; idx < entry_count; ++idx) {
        shell_printf(
            "%s%s\n",
            entries[idx].name,
            (entries[idx].qid.type & M9P_QID_DIR) != 0u ? "/" : "");
    }

    rc = m9p_client_clunk(&shell_m9p_client, fid);
    print_m9p_result("ls", rc);
    return rc;
}

/**
 * @brief 查看文本(cat)命令处理器示例
 * 打印远端文本节点内容。
 */
static int handle_cat(const char *args)
{
    char normalized_path[SHELL_LINE_CAP];
    uint8_t chunk[65];
    struct m9p_open_result open_result;
    uint16_t fid;
    uint32_t offset = 0u;
    int rc;
    bool saw_data = false;

    if (args == NULL || *args == '\0') {
        shell_puts("usage: cat <path>");
        return -1;
    }

    rc = ensure_m9p_session();
    if (rc != 0) {
        print_m9p_result("attach", rc);
        return rc;
    }

    rc = m9p_client_open_path(
        &shell_m9p_client,
        normalize_remote_path(args, normalized_path, sizeof(normalized_path)),
        M9P_OREAD,
        &fid,
        &open_result);
    if (rc != 0) {
        print_m9p_result("cat open", rc);
        return rc;
    }

    do {
        uint16_t count = 64u;

        rc = m9p_client_read(&shell_m9p_client, fid, offset, chunk, &count);
        if (rc != 0) {
            (void)m9p_client_clunk(&shell_m9p_client, fid);
            print_m9p_result("cat read", rc);
            return rc;
        }
        if (count == 0u) {
            break;
        }
        chunk[count] = '\0';
        shell_emit_text((const char *)chunk);
        saw_data = true;
        offset += count;
        if (count < 64u) {
            break;
        }
    } while (true);

    if (saw_data && offset > 0u && chunk[(offset - 1u) % 64u] != '\n') {
        shell_emit_text("\n");
    }

    rc = m9p_client_clunk(&shell_m9p_client, fid);
    print_m9p_result("cat", rc);
    return rc;
}

static int handle_write(const char *args)
{
    char buffer[SHELL_LINE_CAP];
    char normalized_path[SHELL_LINE_CAP];
    char *path;
    char *value;
    struct m9p_open_result open_result;
    uint16_t fid;
    uint16_t written = 0u;
    int rc;

    if (args == NULL || *args == '\0') {
        shell_puts("usage: write <path> <text>");
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", args);
    path = skip_spaces(buffer);
    value = path;
    while (*value != '\0' && *value != ' ' && *value != '\t') {
        ++value;
    }
    if (*value == '\0') {
        shell_puts("usage: write <path> <text>");
        return -1;
    }
    *value++ = '\0';
    value = skip_spaces(value);
    if (*value == '\0') {
        shell_puts("usage: write <path> <text>");
        return -1;
    }

    rc = ensure_m9p_session();
    if (rc != 0) {
        print_m9p_result("attach", rc);
        return rc;
    }

    rc = m9p_client_open_path(
        &shell_m9p_client,
        normalize_remote_path(path, normalized_path, sizeof(normalized_path)),
        M9P_ORDWR,
        &fid,
        &open_result);
    if (rc != 0) {
        print_m9p_result("write open", rc);
        return rc;
    }

    rc = m9p_client_write(
        &shell_m9p_client,
        fid,
        0u,
        (const uint8_t *)value,
        (uint16_t)strlen(value),
        &written);
    if (rc != 0) {
        (void)m9p_client_clunk(&shell_m9p_client, fid);
        print_m9p_result("write data", rc);
        return rc;
    }

    rc = m9p_client_clunk(&shell_m9p_client, fid);
    shell_printf("write: wrote %u bytes\n", (unsigned)written);
    print_m9p_result("write", rc);
    return rc;
}

static int handle_stat(const char *args)
{
    char normalized_path[SHELL_LINE_CAP];
    struct m9p_open_result open_result;
    struct m9p_stat stat_info;
    uint16_t fid;
    int rc;

    if (args == NULL || *args == '\0') {
        shell_puts("usage: stat <path>");
        return -1;
    }

    rc = ensure_m9p_session();
    if (rc != 0) {
        print_m9p_result("attach", rc);
        return rc;
    }

    rc = m9p_client_open_path(
        &shell_m9p_client,
        normalize_remote_path(args, normalized_path, sizeof(normalized_path)),
        M9P_OREAD,
        &fid,
        &open_result);
    if (rc != 0) {
        print_m9p_result("stat open", rc);
        return rc;
    }

    rc = m9p_client_stat(&shell_m9p_client, fid, &stat_info);
    if (rc == 0) {
        shell_printf(
            "name=%s size=%lu perm=0x%02x flags=0x%02x\n",
            stat_info.name,
            (unsigned long)stat_info.size,
            (unsigned)stat_info.perm,
            (unsigned)stat_info.flags);
        print_qid(&stat_info.qid);
    }
    (void)m9p_client_clunk(&shell_m9p_client, fid);
    print_m9p_result("stat", rc);
    return rc;
}

/**
 * @brief attach 会话连接测试处理器
 * 用以请求远端 mini9p 服务器开启通信根节点，设置数据包基础大小和配置。
 */
static int handle_m9p_attach(const char *args)
{
    int rc;

    (void)args;

    rc = ensure_m9p_client_initialized();
    if (rc != 0) {
        print_m9p_result("init", rc);
        return rc;
    }

    rc = m9p_client_attach(&shell_m9p_client, M9P_CLIENT_BUFFER_CAP, 1u, 0u);
    if (rc == 0) {
        shell_printf(
            "attach: msize=%u max_fids=%u feature_bits=0x%08lx\n",
            (unsigned)shell_m9p_client.negotiated_msize,
            (unsigned)shell_m9p_client.max_fids,
            (unsigned long)shell_m9p_client.feature_bits);
        print_qid(&shell_m9p_client.root_qid);
    }
    print_m9p_result("attach", rc);
    return rc;
}

/**
 * @brief walk 路径节点申请测试处理器
 * walk 用于让 mini9p 根据传入路径（如 `/dev/led`），动态分配新的 fid，获取对端虚拟资源，属于 VFS 和远程资源建立映射的核心方法。
 */
static int handle_m9p_walk(const char *args)
{
    char normalized_path[SHELL_LINE_CAP];
    if (args == NULL || *args == '\0') {
        shell_puts("usage: m9p_walk <path>");
        return -1;
    }

    {
        uint16_t fid = 0u;
        struct m9p_qid qid;
        int rc = ensure_m9p_session();

        if (rc != 0) {
            print_m9p_result("attach", rc);
            return rc;
        }

        rc = m9p_client_walk_path(
            &shell_m9p_client,
            normalize_remote_path(args, normalized_path, sizeof(normalized_path)),
            &fid,
            &qid);
        if (rc == 0) {
            shell_printf("walk: fid=%u\n", (unsigned)fid);
            print_qid(&qid);
            (void)m9p_client_clunk(&shell_m9p_client, fid);
        }
        print_m9p_result("walk", rc);
        return rc;
    }
}

/**
 * @brief shell 命令行执行入口
 * 解析一行字符串，将空格前的字段作为命令类型参数提取，同时保留空格后的原始字符串作为 args 传递至下层对应处理流内。
 * 能够满足基本单传参交互需求，若有需要也可以改为 argc/argv 更通用的形式。
 */
int shell_execute_line(const char *line)
{
    char buffer[SHELL_LINE_CAP];
    char *command;
    char *args;

    if (line == NULL) {
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", line);
    trim_line(buffer);
    command = skip_spaces(buffer);
    if (*command == '\0') {
        return 0;
    }

    /* 将命令的第一个单词与后面的参数进行就地分割。
     * 对于这种最简易版的 shell，这样足以应付单参数命令（例如路径）。
     */
    args = command;
    while (*args != '\0' && *args != ' ' && *args != '\t') {
        ++args;
    }
    if (*args != '\0') {
        *args++ = '\0';
        args = skip_spaces(args);
    }

    /* 故意设计的精简命令集，为了满足以下前期验证工作：
     * 1. 验证 shell 的 I/O 及交互逻辑
     * 2. Lua 的内联/打包执行能力
     * 3. 对 m9p 的各类操作探测等项目级钩子
     */
    if (strcmp(command, "ls") == 0) {
        return handle_ls(args);
    }
    if (strcmp(command, "cat") == 0) {
        return handle_cat(args);
    }
    if (strcmp(command, "write") == 0) {
        return handle_write(args);
    }
    if (strcmp(command, "stat") == 0) {
        return handle_stat(args);
    }
    if (strcmp(command, "echo") == 0) {
        return handle_echo(args);
    }
    if (strcmp(command, "m9p_attach") == 0) {
        return handle_m9p_attach(args);
    }
    if (strcmp(command, "m9p_walk") == 0) {
        return handle_m9p_walk(args);
    }
    if (strcmp(command, "set_transport") == 0) {
        return handle_set_transport(args);
    }
    if (strcmp(command, "help") == 0) {
        print_banner();
        return 0;
    }
    if (strcmp(command, "heap") == 0) {
        shell_printf("free heap: %u bytes\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return 0;
    }
    if (strcmp(command, "lua") == 0) {
        return handle_lua(args);
    }
    if (strcmp(command, "m9p") == 0) {
        return handle_m9p(args);
    }
    if (strcmp(command, "reboot") == 0) {
        shell_puts("restarting...");
        fflush(stdout);
        esp_restart();
        return 0;
    }

    shell_printf("unknown command: %s\n", command);
    return -1;
}

/**
 * @brief 系统启动时的默认交互钩子
 * 最开始打印一份帮助菜单，紧接着执行一次内部置备的 Lua 剧本演示功能。
 */
void shell_run_boot_demo(void)
{
    /* Show the available commands, then immediately exercise the Lua path
     * so a fresh boot already demonstrates useful behavior.
     */
    print_banner();
    shell_puts("[boot] running Lua demo...");
    (void)shell_execute_line("lua");
    shell_puts("");
}

/**
 * @brief 壳层无限交互死循环任务（供 FreeRTOS 调度使用）
 * 不断调用 fgets 获取输入并喂入执行器中。
 */
static void shell_task(void *arg)
{
    char line[SHELL_LINE_CAP];

    (void)arg;

    /* 我们复用了 ESP-IDF 提供的标准输入/输出流体系。
     * 因此这个无限循环可以非常符合经典终端的表现，阻塞态地等待 I/O 到来。
     */
    for (;;) {
        shell_printf("pwos> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        (void)shell_execute_line(line);
    }
}

/**
 * @brief Shell 初始化并启动多线程调度
 * 防止如果在其它地方重复调用导致的重复创立。
 */
void shell_start(void)
{
    static bool started;

    /* Guard against starting two shell tasks if app_main is refactored later. */
    if (started) {
        return;
    }

    started = true;
    xTaskCreate(shell_task, "pw_shell", SHELL_TASK_STACK_SIZE, NULL, SHELL_TASK_PRIORITY, NULL);
}
