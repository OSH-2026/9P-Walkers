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
#include "mini9p_protocol.h"

#define SHELL_TASK_STACK_SIZE 4096
#define SHELL_TASK_PRIORITY 5
#define SHELL_LINE_CAP 256

/* Tiny helpers for in-place command parsing. We keep parsing local to
 * this file so the shell stays easy to replace later.
 */
static char *skip_spaces(char *text)
{
    while (*text == ' ' || *text == '\t') {
        ++text;
    }

    return text;
}

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

static void print_banner(void)
{
    puts("");
    puts("pwos shell");
    puts("  help      show commands");
    puts("  heap      print free heap");
    puts("  lua       run built-in Lua demo");
    puts("  lua ...   execute inline Lua chunk");
    puts("  m9p N     print mini9p error name");
    puts("  reboot    restart the board");
    puts("");
}

static int handle_lua(const char *args)
{
    /* Bare "lua" runs the canned demo; "lua <chunk>" executes inline code. */
    if (args == NULL || *args == '\0') {
        return pw_lua_run_demo();
    }

    return pw_lua_run_buffer("shell", args);
}

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

    /* Split the first token from the rest of the line in place,
     * which is enough for this minimal shell.
     */
    args = command;
    while (*args != '\0' && *args != ' ' && *args != '\t') {
        ++args;
    }
    if (*args != '\0') {
        *args++ = '\0';
        args = skip_spaces(args);
    }

    /* The command set is intentionally tiny for bring-up: enough to
     * prove shell I/O, Lua execution, and one project-specific hook.
     */
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

static void shell_task(void *arg)
{
    char line[SHELL_LINE_CAP];

    (void)arg;

    /* stdin/stdout are provided by the ESP-IDF console, so this task can
     * behave like a normal blocking REPL loop.
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
