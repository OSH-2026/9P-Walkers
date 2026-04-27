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

#ifdef __cplusplus
}
#endif

#endif
