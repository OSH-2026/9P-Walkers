/**
 * @file pwos_log.h
 * @brief Global fixed-size ring log for PWOS node diagnostics.
 */

#ifndef PWOS_LOG_H
#define PWOS_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_LOG_EVENT_GENERIC 1u
#define PWOS_LOG_EVENT_MESH_SEND 2u
#define PWOS_LOG_EVENT_MESH_SEND_PORT 3u

void pwos_log_init(void);
void pwos_log_event(uint8_t event, uint32_t a, uint32_t b, uint32_t c, int rc);
void pwos_log_mesh_tx(
    uint8_t event,
    uint8_t type,
    uint8_t src,
    uint8_t dst,
    uint8_t next_hop,
    uint8_t port_id,
    uint16_t len,
    uint8_t mini9p_type,
    int rc);
int pwos_log_format(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif
