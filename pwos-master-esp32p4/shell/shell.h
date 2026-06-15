#ifndef PWOS_MASTER_SHELL_H
#define PWOS_MASTER_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the background task that serves the UART shell prompt. */
void shell_start(void);
/* Run the built-in boot sequence once before entering interactive mode. */
void shell_run_boot_demo(void);
/* Parse and execute a single command line. */
int shell_execute_line(const char *line);
/* Write text to stdout and the optional shell output hook. */
void shell_write(const char *text);

/*
 * Optional output hook: when set, every line of text printed by the shell
 * (printf / puts) is also forwarded to this callback as a NUL-terminated
 * string. Used by the WebSocket bridge to mirror shell output to browsers.
 * Pass NULL to remove the hook. Not thread-safe on its own — callers must
 * ensure exclusive access when setting and clearing.
 */
typedef void (*shell_print_hook_t)(const char *text);
void shell_set_print_hook(shell_print_hook_t hook);

#ifdef __cplusplus
}
#endif

#endif
