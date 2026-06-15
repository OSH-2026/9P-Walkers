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
 * attach to esp_netif, obtain an IPv4 address from the router via DHCP,
 * and start the driver.
 *
 * Default config uses ETH_ESP32_EMAC_DEFAULT_CONFIG() which is
 * tuned for the ESP32-P4 dev board RMII pinout.
 *
 * mDNS is also announced so the server is reachable as
 * http://pwos.local when the host OS supports it.
 */
void lan_init(void);

/*
 * Copy the current Ethernet IP into `buf` as a dotted-quad string.
 * Returns false (and writes an empty string) while no IP is held yet.
 */
bool lan_get_ip_str(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
