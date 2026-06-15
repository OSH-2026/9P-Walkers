#include "shell.h"

#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cluster_host_vfs.h"
#include "cluster_config.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lan_init.h"
#include "lua_port.h"
#include "mesh_wifi_link.h"
#include "mini9p_client.h"
#include "mini9p_protocol.h"

#define SHELL_TASK_STACK_SIZE 4096
#define SHELL_TASK_PRIORITY 5
#define SHELL_LINE_CAP 256
#define SHELL_EMIT_CAP 512

/* Optional hook for mirroring shell output to an external sink (e.g. WS). */
static shell_print_hook_t s_print_hook = NULL;

void shell_set_print_hook(shell_print_hook_t hook)
{
    s_print_hook = hook;
}

/* Emit pre-formatted text to stdout AND the optional hook. */
void shell_write(const char *text)
{
    fputs(text, stdout);
    if (s_print_hook) {
        s_print_hook(text);
    }
}

/* printf-style wrapper that routes through shell_write. */
static void shell_printf(const char *fmt, ...)
{
    char buf[SHELL_EMIT_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    shell_write(buf);
}

/* puts-style wrapper (appends '\n' like puts does). */
static void shell_puts(const char *s)
{
    char buf[SHELL_EMIT_CAP];
    snprintf(buf, sizeof(buf), "%s\n", s);
    shell_write(buf);
}

/* 用于测试 Shell 命令操作的模拟 m9p 客户端环境 */
static struct m9p_client shell_m9p_client;
static bool shell_m9p_initialized = false;

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
    shell_printf("[mock m9p] Transmitting %u bytes...\n", (unsigned)tx_len);
    *rx_len = 0;
    return 0;
}

/**
 * @brief 确保模拟的 m9p 客户端已经初始化
 * 以免在输入 shell 命令时出现未分配内存被访问的宕机问题
 */
static void ensure_m9p_mock_client(void)
{
    if (!shell_m9p_initialized) {
        m9p_client_init(&shell_m9p_client, mock_transport, NULL);
        shell_m9p_initialized = true;
    }
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
    shell_puts("  help      显示命令菜单(show commands)");
    shell_puts("  heap      打印可用堆内存信息(print free heap)");
    shell_puts("  echo ...  回显文本或写入文件(print text / echo .. > path)");
    shell_puts("  ls [dir]  枚举集群目录节点内容(list cluster directory)");
    shell_puts("  cat <file>读取集群节点文件内容(read cluster file)");
    shell_puts("  lua       执行内置测试用Lua剧本(run built-in Lua demo)");
    shell_puts("  lua ...   执行随后输入的一行Lua语句(execute inline Lua chunk)");
    shell_puts("  m9p_attach 本地 mock 自检；实物节点请用 ls / 和 cat /mcuN/...");
    shell_puts("  m9p_walk  测试根据路径分配ID(test mini9p walk)");
    shell_puts("  m9p N     查阅 mini9p 状态码名称(print mini9p error name)");
    shell_puts("  mesh      打印主机本地 mesh 节点/链路/路由(print local mesh state)");
    shell_puts("  wifi ...  LAN/TCP mesh 透传管理(wifi status|start [port]|stop)");
    shell_puts("  reboot    系统重启(restart the board)");
    shell_puts("");
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
 *
 * 支持两种用法：
 *   echo <text>            将文本原样回显到终端
 *   echo <text> > <path>   将文本写入集群路径对应文件（例如 echo 100 > /mcu1/motor/speed）
 *
 * 重定向时文本与路径都会去掉首尾空白；文本按原样写入，不追加换行。
 */
static int handle_echo(const char *args)
{
    if (args == NULL) {
        shell_puts("");
        return 0;
    }

    /* 无重定向符时退回普通回显。 */
    const char *redir = strchr(args, '>');
    if (redir == NULL) {
        shell_puts(args);
        return 0;
    }

    /* 复制一份可修改副本，便于就地裁切文本与路径两侧空白。 */
    char work[SHELL_LINE_CAP];
    snprintf(work, sizeof(work), "%s", args);

    char *sep = work + (redir - args);
    *sep = '\0'; /* 在 '>' 处把文本与路径切开 */

    char *text = work;
    char *path = sep + 1;

    /* 去掉文本尾部空白。 */
    size_t text_len = strlen(text);
    while (text_len > 0u && (text[text_len - 1u] == ' ' || text[text_len - 1u] == '\t')) {
        text[--text_len] = '\0';
    }

    /* 跳过路径前可能多出的 '>'（容忍 ">>"，当前按覆盖写处理）与前导空白。 */
    while (*path == '>') {
        ++path;
    }
    path = skip_spaces(path);

    /* 去掉路径尾部空白。 */
    size_t path_len = strlen(path);
    while (path_len > 0u && (path[path_len - 1u] == ' ' || path[path_len - 1u] == '\t')) {
        path[--path_len] = '\0';
    }

    if (*path == '\0') {
        shell_puts("usage: echo <text> > <path>");
        return -1;
    }

    uint16_t written = 0u;
    int rc = cluster_vfs_write_path(path, (const uint8_t *)text,
                                    (uint16_t)text_len, &written);
    if (rc != 0) {
        shell_printf("echo: error %d (%s)\n", rc, m9p_error_name((uint16_t)(-rc)));
        return rc;
    }

    shell_printf("wrote %u byte(s) to %s\n", (unsigned)written, path);
    return 0;
}

static int handle_ls(const char *args)
{
    const char *path = (args != NULL && *args != '\0') ? args : "/";
    struct m9p_dirent entries[32];
    size_t count = 0;
    int rc;

    shell_printf("ls %s\n", path);
    rc = cluster_vfs_list(path, entries, sizeof(entries) / sizeof(entries[0]), &count);
    if (rc != 0) {
        shell_printf("ls: error %d (%s)\n", rc, m9p_error_name((uint16_t)(-rc)));
        return rc;
    }
    if (count == 0) {
        shell_puts("(empty)");
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        bool is_dir = (entries[i].qid.type & M9P_QID_DIR) != 0;
        shell_printf("  %s%s\n", entries[i].name, is_dir ? "/" : "");
    }
    return 0;
}

static int handle_cat(const char *args)
{
    if (args == NULL || *args == '\0') {
        shell_puts("usage: cat <path>");
        return -1;
    }

    uint8_t buf[256];
    uint16_t len = sizeof(buf) - 1u;
    int rc = cluster_vfs_read_path(args, buf, &len);
    if (rc != 0) {
        shell_printf("cat: error %d (%s)\n", rc, m9p_error_name((uint16_t)(-rc)));
        return rc;
    }
    buf[len] = '\0';
    shell_printf("%s", (char *)buf);
    if (len > 0 && buf[len - 1u] != '\n') {
        shell_puts("");
    }
    return 0;
}

static const char *mesh_mode_name(enum cluster_mode mode)
{
    switch (mode) {
    case CLUSTER_MODE_DIRECT_TABLE:
        return "direct";
    case CLUSTER_MODE_TOPOLOGY:
        return "topology";
    default:
        return "?";
    }
}

static int handle_mesh(const char *args)
{
    struct cluster *mesh_cluster;
    int rc;

    if (args != NULL && *args != '\0' && strcmp(args, "status") != 0) {
        shell_puts("usage: mesh [status]");
        return -1;
    }

    mesh_cluster = cluster_config_mesh_cluster();
    if (mesh_cluster == NULL || !mesh_cluster->initialized) {
        shell_puts("mesh: cluster not initialized");
        return -(int)M9P_ERR_EAGAIN;
    }

    rc = cluster_rebuild_routes(mesh_cluster);
    shell_printf(
        "mesh local=0x%02x mode=%s rebuild=%d dirty=%u\n",
        mesh_cluster->config.local_addr,
        mesh_mode_name(mesh_cluster->config.mode),
        rc,
        mesh_cluster->routes_dirty ? 1u : 0u);

    shell_puts("nodes:");
    bool any = false;
    for (size_t i = 0u; i < CLUSTER_MAX_NODES; ++i) {
        const struct cluster_node *node = &mesh_cluster->nodes[i];

        if (!node->valid) {
            continue;
        }
        any = true;
        shell_printf(
            "  addr=0x%02x online=%u caps=0x%04x ports=0x%02x wifi=%u\n",
            node->addr,
            node->online ? 1u : 0u,
            node->capability_bits,
            node->port_bitmap,
            node->wifi_supported ? 1u : 0u);
    }
    if (!any) {
        shell_puts("  (none)");
    }

    shell_puts("links:");
    any = false;
    for (size_t i = 0u; i < CLUSTER_MAX_LINKS; ++i) {
        const struct cluster_link *link = &mesh_cluster->links[i];

        if (!link->valid) {
            continue;
        }
        any = true;
        shell_printf(
            "  0x%02x -> 0x%02x metric=%u bidi=%u from_port=%u to_port=%u\n",
            link->from,
            link->to,
            link->metric,
            link->bidirectional ? 1u : 0u,
            link->from_port,
            link->to_port);
    }
    if (!any) {
        shell_puts("  (none)");
    }

    shell_puts("routes:");
    any = false;
    for (size_t i = 0u; i < CLUSTER_MAX_ROUTES; ++i) {
        const struct cluster_route *route = &mesh_cluster->routes[i];

        if (!route->valid) {
            continue;
        }
        any = true;
        shell_printf(
            "  dst=0x%02x next=0x%02x metric=%u local=%u selector=%s\n",
            route->dst,
            route->next_hop,
            route->metric,
            route->local ? 1u : 0u,
            route->selector_is_port ? "port" : "addr");
    }
    if (!any) {
        shell_puts("  (none)");
    }

    return rc;
}

/**
 * @brief attach 会话连接测试处理器
 * 用以请求远端 mini9p 服务器开启通信根节点，设置数据包基础大小和配置。
 */
static int handle_m9p_attach(const char *args)
{
    shell_puts("m9p_attach is a local mock self-test; it does not attach to a real STM32 slave.");
    shell_puts("Use 'ls /' to see registered nodes, then 'cat /mcu1/sys/health' or another /mcuN path.");
    ensure_m9p_mock_client();
    (void)args;

    int rc = m9p_client_attach(&shell_m9p_client, 256, 1, 0);
    shell_printf("mock attach result: %d\n", rc);
    return rc;
}

/**
 * @brief walk 路径节点申请测试处理器
 * walk 用于让 mini9p 根据传入路径（如 `/dev/led`），动态分配新的 fid，获取对端虚拟资源，属于 VFS 和远程资源建立映射的核心方法。
 */
static int handle_m9p_walk(const char *args)
{
    if (args == NULL || *args == '\0') {
        shell_puts("usage: m9p_walk <path>");
        return -1;
    }
    shell_printf("Walking to %s via mock mini9p...\n", args);
    ensure_m9p_mock_client();

    uint16_t fid;
    struct m9p_qid qid;
    int rc = m9p_client_walk_path(&shell_m9p_client, args, &fid, &qid);
    shell_printf("mock walk path rc=%d, matched fid=%u\n", rc, fid);
    return rc;
}

/**
 * @brief wifi 命令处理器：管理路由器局域网 TCP mesh 透传链路。
 *
 * 用法：
 *   wifi / wifi status      显示以太网 DHCP IP 与 TCP mesh 链路状态
 *   wifi start [tcp_port]   启动 TCP 监听（默认端口 9000）
 *   wifi stop               停止监听并断开 client
 *
 * 主机经网线接入路由器；从机侧 TCP/WiFi 透传模块以 TCP client 模式
 * 连接 <主机IP>:<端口>，mesh 帧在该连接上原样透传。
 */
static int handle_wifi(const char *args)
{
    char ip[16];
    char status[256];

    if (args == NULL || *args == '\0' || strcmp(args, "status") == 0) {
        if (lan_get_ip_str(ip, sizeof(ip))) {
            shell_printf("eth ip   : %s (http://" LAN_MDNS_HOSTNAME ".local)\n", ip);
        } else {
            shell_puts("eth ip   : (waiting for DHCP)");
        }
        if (mesh_wifi_link_format_status(status, sizeof(status)) > 0) {
            shell_write(status);
        }
        return 0;
    }

    if (strncmp(args, "start", 5u) == 0) {
        const char *port_text = args + 5;
        long port = (long)MESH_WIFI_LINK_DEFAULT_TCP_PORT;
        int rc;

        while (*port_text == ' ' || *port_text == '\t') {
            ++port_text;
        }
        if (*port_text != '\0') {
            char *end = NULL;

            port = strtol(port_text, &end, 0);
            if (end == port_text || port <= 0 || port > 65535) {
                shell_puts("usage: wifi start [tcp_port]");
                return -1;
            }
        }

        rc = mesh_wifi_link_start((uint16_t)port);
        if (rc != 0) {
            shell_printf("wifi start failed: %d\n", rc);
            return rc;
        }

        shell_printf("lan tcp mesh link listening on tcp port %ld\n", port);
        if (lan_get_ip_str(ip, sizeof(ip))) {
            shell_printf("slave transparent module should connect to %s:%ld\n", ip, port);
        }
        return 0;
    }

    if (strcmp(args, "stop") == 0) {
        int rc = mesh_wifi_link_stop();

        if (rc != 0) {
            shell_printf("wifi stop failed: %d\n", rc);
            return rc;
        }
        shell_puts("lan tcp mesh link stopped");
        return 0;
    }

    shell_puts("usage: wifi [status|start [tcp_port]|stop]");
    return -1;
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
    for (char *p = command; *p != '\0'; ++p) {
        *p = (char)tolower((unsigned char)*p);
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
    if (strcmp(command, "echo") == 0) {
        return handle_echo(args);
    }
    if (strcmp(command, "m9p_attach") == 0) {
        return handle_m9p_attach(args);
    }
    if (strcmp(command, "m9p_walk") == 0) {
        return handle_m9p_walk(args);
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
    if (strcmp(command, "mesh") == 0) {
        return handle_mesh(args);
    }
    if (strcmp(command, "wifi") == 0) {
        return handle_wifi(args);
    }
    if (strcmp(command, "reboot") == 0) {
        shell_puts("restarting...");
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
        /* Prompt only to UART (not the WS hook) — the browser has its own echo. */
        fputs("pwos> ", stdout);
        fflush(stdout);

        while (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
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
