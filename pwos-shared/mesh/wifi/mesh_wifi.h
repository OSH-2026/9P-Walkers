#ifndef MESH_WIFI_H
#define MESH_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../envelope/mesh_protocal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*mesh_wifi_send_raw_fn)(void *io_ctx, const uint8_t *tx_data, size_t tx_len);
typedef int (*mesh_wifi_receive_raw_fn)(void *io_ctx, uint8_t *rx_data, size_t rx_cap, size_t *rx_len);

struct mesh_wifi_config {
    mesh_wifi_send_raw_fn send_frame;
    mesh_wifi_receive_raw_fn receive_frame;
    void *io_ctx;
    uint32_t io_timeout_ms;
};

struct mesh_wifi {
    struct mesh_wifi_config config;
    bool initialized;
};

void mesh_wifi_get_default_config(struct mesh_wifi_config *out_config);

int mesh_wifi_init(
    struct mesh_wifi *wifi,
    const struct mesh_wifi_config *config);

void mesh_wifi_deinit(struct mesh_wifi *wifi);

int mesh_wifi_send_frame(
    struct mesh_wifi *wifi,
    const uint8_t *tx_data,
    size_t tx_len);

int mesh_wifi_receive_frame(
    struct mesh_wifi *wifi,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len);

#ifdef __cplusplus
}
#endif

#endif