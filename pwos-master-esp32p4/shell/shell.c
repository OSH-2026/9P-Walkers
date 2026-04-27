#include "shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_port.h"
#include "mini9p_client.h"
#include "mini9p_protocol.h"

#define SHELL_TASK_STACK_SIZE 4096
#define SHELL_TASK_PRIORITY 5
#define SHELL_LINE_CAP 256

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
    printf("[mock m9p] Transmitting %u bytes...\n", (unsigned)tx_len);
    *rx_len = 0; /* 伪造空的返回数据 */
    return 0;    /* 返回成功 */
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
    puts("");
    puts("pwos shell");
    puts("  help      显示命令菜单(show commands)");
    puts("  heap      打印可用堆内存信息(print free heap)");
    puts("  echo ...  回显输入文本(print text)");
    puts("  ls <dir>  模拟枚举目录节点内容(list directory - mock m9p)");
    puts("  cat <file>模拟打印节点文件(read file - mock m9p)");
    puts("  lua       执行内置测试用Lua剧本(run built-in Lua demo)");
    puts("  lua ...   执行随后输入的一行Lua语句(execute inline Lua chunk)");
    puts("  m9p_attach 测试向客户端服务端发起链接(test mini9p attach)");
    puts("  m9p_walk  测试根据路径分配ID(test mini9p walk)");
    puts("  m9p N     查阅 mini9p 状态码名称(print mini9p error name)");
    puts("  reboot    系统重启(restart the board)");
    puts("");
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
        puts("usage: m9p <code>");
        return -1;
    }

    code = strtol(args, &end, 0);
    if (end == args) {
        puts("invalid code");
        return -1;
    }

    printf("m9p[%ld] = %s\n", code, m9p_error_name((uint16_t)code));
    return 0;
}

/**
 * @brief 输出(echo)命令处理器
 * 将用户敲入的文本原样输出，常用于测试 shell 解析行为是否正确包含空格。
 */
static int handle_echo(const char *args)
{
    if (args == NULL) {
        puts("");
        return 0;
    }
    puts(args);
    return 0;
}

/**
 * @brief 列表(ls)命令处理器示例
 * 模拟获取目录结构的操作。在此测试阶段中：
 *  1. 保证了在未连接从机的情况下，初始化本地 Mock 传输层；
 *  2. 构造一次 `m9p_client_open_path` (使用 M9P_OREAD 只读标志)，
 *     向目标文件描述符发起申请。
 *  3. 操作结束后如果通过，则关闭释放该 fid。
 */
static int handle_ls(const char *args)
{
    if (args == NULL || *args == '\0') {
        puts("usage: ls <path>");
        return -1;
    }
    printf("Listing path %s...\n", args);
    ensure_m9p_mock_client();
    
    /* 仅仅为了测试执行目录开启的打包请求流程 */
    uint16_t fid;
    struct m9p_open_result out_res;
    int rc = m9p_client_open_path(&shell_m9p_client, args, M9P_OREAD, &fid, &out_res);
    
    if (rc == 0) {
        printf("Successfully opened path: %s (fid=%u)\n", args, fid);
        m9p_client_clunk(&shell_m9p_client, fid);
    } else {
        printf("Mock ls failed (expected without real transport): rc=%d\n", rc);
    }
    return 0;
}

/**
 * @brief 查看文本(cat)命令处理器示例
 * 发起模拟的文件内容读取操作。在此阶段中：
 *  1. 发送文件句柄 open 请求；
 *  2. 发起基于该 `fid` 以 0 个字节偏移去发起的 `read` 请求，同时准备接收最多 64B 的模拟缓冲区；
 *  3. 当所有流程完成后执行 clunk 取消挂接。
 */
static int handle_cat(const char *args)
{
    if (args == NULL || *args == '\0') {
        puts("usage: cat <path>");
        return -1;
    }
    printf("Reading file %s...\n", args);
    ensure_m9p_mock_client();

    uint16_t fid;
    struct m9p_open_result out_res;
    int rc = m9p_client_open_path(&shell_m9p_client, args, M9P_OREAD, &fid, &out_res);
    if (rc == 0) {
        uint8_t buf[64];
        uint16_t count = sizeof(buf);
        rc = m9p_client_read(&shell_m9p_client, fid, 0, buf, &count);
        if (rc == 0) {
            printf("Mock cat read %u bytes.\n", count);
        }
        m9p_client_clunk(&shell_m9p_client, fid);
    } else {
         printf("Mock cat failed (expected without real transport): rc=%d\n", rc);
    }
    return 0;
}

/**
 * @brief attach 会话连接测试处理器
 * 用以请求远端 mini9p 服务器开启通信根节点，设置数据包基础大小和配置。
 */
static int handle_m9p_attach(const char *args)
{
    printf("Attaching to mock mini9p server...\n");
    ensure_m9p_mock_client();
    (void)args;
    
    int rc = m9p_client_attach(&shell_m9p_client, 256, 1, 0);
    printf("mock attach result: %d\n", rc);
    return rc;
}

/**
 * @brief walk 路径节点申请测试处理器
 * walk 用于让 mini9p 根据传入路径（如 `/dev/led`），动态分配新的 fid，获取对端虚拟资源，属于 VFS 和远程资源建立映射的核心方法。
 */
static int handle_m9p_walk(const char *args)
{
    if (args == NULL || *args == '\0') {
        puts("usage: m9p_walk <path>");
        return -1;
    }
    printf("Walking to %s via mock mini9p...\n", args);
    ensure_m9p_mock_client();

    uint16_t fid;
    struct m9p_qid qid;
    int rc = m9p_client_walk_path(&shell_m9p_client, args, &fid, &qid);
    printf("mock walk path rc=%d, matched fid=%u\n", rc, fid);
    return rc;
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
        printf("free heap: %u bytes\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return 0;
    }
    if (strcmp(command, "lua") == 0) {
        return handle_lua(args);
    }
    if (strcmp(command, "m9p") == 0) {
        return handle_m9p(args);
    }
    if (strcmp(command, "reboot") == 0) {
        puts("restarting...");
        fflush(stdout);
        esp_restart();
        return 0;
    }

    printf("unknown command: %s\n", command);
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
    puts("[boot] running Lua demo...");
    (void)shell_execute_line("lua");
    puts("");
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
        printf("pwos> ");
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
