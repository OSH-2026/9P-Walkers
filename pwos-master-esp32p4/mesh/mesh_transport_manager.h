#ifndef PWOS_MASTER_MESH_TRANSPORT_MANAGER_H
#define PWOS_MASTER_MESH_TRANSPORT_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mesh_transport_manager_init_default(void);
void mesh_transport_manager_deinit_default(void);
void *mesh_transport_manager_default(void);

int mesh_transport_manager_send_frame(
    void *transport_ctx,
    uint8_t next_hop,
    const uint8_t *tx_data,
    size_t tx_len);

int mesh_transport_manager_receive_frame(
    void *transport_ctx,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_MASTER_MESH_TRANSPORT_MANAGER_H */
