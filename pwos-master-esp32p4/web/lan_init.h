#ifndef PWOS_MASTER_LAN_INIT_H
#define PWOS_MASTER_LAN_INIT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mDNS hostname — the board is reachable as http://pwos.local on the LAN. */
#define LAN_MDNS_HOSTNAME "pwos"

/*
 * Initialize Ethernet (internal EMAC + RMII PHY) on ESP32-P4,
 * attach to esp_netif with DHCP, and start the driver.
 *
 * Default config uses ETH_ESP32_EMAC_DEFAULT_CONFIG() which is
 * tuned for the ESP32-P4 dev board RMII pinout.
 *
 * After DHCP completes the obtained IP is logged so the user can
 * reach the HTTP/WebSocket server. mDNS is also announced so the
 * server is reachable as http://pwos.local without knowing the IP.
 */
void lan_init(void);

/*
 * Copy the current DHCP-assigned IP into `buf` as a dotted-quad string.
 * Returns false (and writes an empty string) while no IP is held yet.
 */
bool lan_get_ip_str(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
