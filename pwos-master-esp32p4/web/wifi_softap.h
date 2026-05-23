#ifndef PWOS_MASTER_WIFI_SOFTAP_H
#define PWOS_MASTER_WIFI_SOFTAP_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise NVS, netif, and the event loop, then start the ESP32-P4 as a
 * WiFi Access Point so browsers can reach the Web Shell without a router.
 *
 * Default AP:  SSID "9P-Walkers"  Password "pwos1234"
 * Default IP:  192.168.4.1  (matches the fallback in index.html)
 */
void wifi_softap_init(void);

#ifdef __cplusplus
}
#endif

#endif
