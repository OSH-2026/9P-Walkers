#include "mesh_wifi.h"

#include <string.h>

void mesh_wifi_get_default_config(struct mesh_wifi_config *out_config)
{
    if (out_config == NULL) {
        return;
    }

    memset(out_config, 0, sizeof(*out_config));
}

int mesh_wifi_init(
    struct mesh_wifi *wifi,
    const struct mesh_wifi_config *config)
{
    struct mesh_wifi_config merged_config;

    if (wifi == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    mesh_wifi_get_default_config(&merged_config);
    if (config != NULL) {
        merged_config = *config;
    }
    if (merged_config.send_frame == NULL || merged_config.receive_frame == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    memset(wifi, 0, sizeof(*wifi));
    wifi->config = merged_config;
    wifi->initialized = true;
    return 0;
}

void mesh_wifi_deinit(struct mesh_wifi *wifi)
{
    if (wifi == NULL) {
        return;
    }

    memset(wifi, 0, sizeof(*wifi));
}

int mesh_wifi_send_frame(
    struct mesh_wifi *wifi,
    const uint8_t *tx_data,
    size_t tx_len)
{
    if (wifi == NULL || !wifi->initialized || tx_data == NULL || tx_len == 0u) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return wifi->config.send_frame(wifi->config.io_ctx, tx_data, tx_len);
}

int mesh_wifi_receive_frame(
    struct mesh_wifi *wifi,
    uint8_t *rx_data,
    size_t rx_cap,
    size_t *rx_len)
{
    if (wifi == NULL || !wifi->initialized || rx_data == NULL || rx_len == NULL) {
        return -(int)MESH_ERR_INVALID_STATE;
    }

    return wifi->config.receive_frame(wifi->config.io_ctx, rx_data, rx_cap, rx_len);
}