#ifndef PWOS_HOST_SHELL_RUNTIME_H
#define PWOS_HOST_SHELL_RUNTIME_H

#include "command_service.h"

#ifdef __cplusplus
extern "C" {
#endif

int pwos_host_shell_runtime_build_config(
    pwos_command_service_config_t *out_config);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_SHELL_RUNTIME_H */
