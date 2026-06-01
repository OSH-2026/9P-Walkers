#ifndef PWOS_SLAVE_APP_MESH_DIAG_H
#define PWOS_SLAVE_APP_MESH_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PWOS_ENABLE_MESH_DIAG

void mesh_diag_text(const char *text);
void mesh_diag_kv_u32(const char *key, uint32_t value);
void mesh_diag_send_frame(uint8_t port_id, uint8_t next_hop, size_t tx_len, int rc);
void mesh_diag_recv_frame(uint8_t port_id, size_t rx_len, int rc);

#else

static inline void mesh_diag_text(const char *text)
{
    (void)text;
}

static inline void mesh_diag_kv_u32(const char *key, uint32_t value)
{
    (void)key;
    (void)value;
}

static inline void mesh_diag_send_frame(uint8_t port_id, uint8_t next_hop, size_t tx_len, int rc)
{
    (void)port_id;
    (void)next_hop;
    (void)tx_len;
    (void)rc;
}

static inline void mesh_diag_recv_frame(uint8_t port_id, size_t rx_len, int rc)
{
    (void)port_id;
    (void)rx_len;
    (void)rc;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
