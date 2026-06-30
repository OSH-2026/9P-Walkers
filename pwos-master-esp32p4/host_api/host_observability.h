#ifndef PWOS_HOST_OBSERVABILITY_H
#define PWOS_HOST_OBSERVABILITY_H

#include <stddef.h>
#include <stdint.h>

#include "mini9p_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int pwos_host_observability_read(
    const char *path,
    uint8_t *buf,
    uint16_t *in_out_len);

int pwos_host_observability_list(
    const char *path,
    struct m9p_dirent *entries,
    size_t max_entries,
    size_t *out_count);

int pwos_host_observability_stat(
    const char *path,
    struct m9p_stat *out_stat);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_HOST_OBSERVABILITY_H */
