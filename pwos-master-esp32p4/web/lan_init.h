#ifndef PWOS_MASTER_LAN_INIT_H
#define PWOS_MASTER_LAN_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize Ethernet (internal EMAC + RMII PHY) on ESP32-P4,
 * attach to esp_netif with DHCP, and start the driver.
 *
 * Default config uses ETH_ESP32_EMAC_DEFAULT_CONFIG() which is
 * tuned for the ESP32-P4 dev board RMII pinout.
 *
 * After DHCP completes the obtained IP is logged so the user can
 * reach the HTTP/WebSocket server.
 */
void lan_init(void);

#ifdef __cplusplus
}
#endif

#endif
