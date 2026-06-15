#include "lan_init.h"

#include <stdio.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"

#if __has_include("esp_eth_phy_ip101.h")
#include "esp_eth_phy_ip101.h"
#define PWOS_HAS_IP101_PHY_DRIVER 1
#else
#define PWOS_HAS_IP101_PHY_DRIVER 0
#endif

static const char *TAG = "lan";

static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_glue;

/* Last DHCP-assigned IP as text; empty until IP_EVENT_ETH_GOT_IP fires. */
static char s_ip_str[16];

static void eth_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    uint8_t mac[6] = {0};
    esp_eth_handle_t h = *(esp_eth_handle_t *)data;

    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(h, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "link up — HW %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        s_ip_str[0] = '\0';
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "stopped");
        break;
    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base,
                            int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
    ESP_LOGI(TAG, "got IP %s — web shell at http://%s/ (http://" LAN_MDNS_HOSTNAME ".local)",
             s_ip_str, s_ip_str);
}

bool lan_get_ip_str(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return false;
    }
    snprintf(buf, cap, "%s", s_ip_str);
    return s_ip_str[0] != '\0';
}

/* Announce the board on the LAN so browsers can use http://pwos.local. */
static void mdns_setup(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: 0x%x", err);
        return;
    }
    ESP_ERROR_CHECK(mdns_hostname_set(LAN_MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("9P-Walkers Master"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS up — http://" LAN_MDNS_HOSTNAME ".local");
}

void lan_init(void)
{
    /* Initialize TCP/IP stack and default event loop. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create esp-netif for Ethernet. The default ETH netif runs a DHCP client,
     * so the board can plug into a router and receive an address automatically. */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(netif == NULL ? ESP_FAIL : ESP_OK);
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, LAN_MDNS_HOSTNAME));

    /* MAC + PHY config using ESP-IDF defaults for the ESP32-P4 dev board.
     * See esp_eth_mac_esp.h — RMII, MDC=31, MDIO=52, CLK=50, etc. */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_mac_config_t        mac_cfg  = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t        phy_cfg  = ETH_PHY_DEFAULT_CONFIG();

    /* ESP32-P4-Function-EV-Board specific: IP101GRI PHY, address = 1,
     * reset on GPIO 51.  The dedicated IP101 driver is a separate component
     * in some ESP-IDF releases, so keep generic PHY as a compile-time fallback.
     * ETH_PHY_DEFAULT_CONFIG() uses ESP32 defaults (addr=AUTO, rst=5)
     * which are wrong for P4 and cause intermittent link drops. */
    phy_cfg.phy_addr      = 1;
    phy_cfg.reset_gpio_num = 51;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    ESP_ERROR_CHECK(mac == NULL ? ESP_FAIL : ESP_OK);

#if PWOS_HAS_IP101_PHY_DRIVER
    ESP_LOGI(TAG, "using IP101 PHY driver");
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
#else
    ESP_LOGW(TAG, "IP101 PHY driver not available, using generic 802.3 PHY driver");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
#endif
    ESP_ERROR_CHECK(phy == NULL ? ESP_FAIL : ESP_OK);

    /* Install Ethernet driver. */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &s_eth_handle));

    /* Attach driver to TCP/IP stack. */
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(netif, s_eth_glue));

    /* Register event handlers. */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                &got_ip_handler, NULL));

    /* Start driver. Link negotiation and DHCP begin after this. */
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    mdns_setup();

    ESP_LOGI(TAG,
             "LAN init done, waiting for router DHCP on Ethernet; connect another device to "
             "the same WLAN/LAN and open http://<assigned-ip>/ or http://" LAN_MDNS_HOSTNAME ".local");
}
