#ifndef PWOS_MASTER_SHELL_H
#define PWOS_MASTER_SHELL_H

typedef void (*shell_output_fn)(void *ctx, const char *text);

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the background task that serves the UART shell prompt. */
void shell_start(void);
/* Run the built-in boot sequence once before entering interactive mode. */
void shell_run_boot_demo(void);
/* Parse and execute a single command line. */
int shell_execute_line(const char *line);
/* Mirror shell output to an optional secondary sink, such as WebSocket. */
void shell_set_output_hook(shell_output_fn hook, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
