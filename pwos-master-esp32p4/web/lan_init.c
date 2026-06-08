#include "lan_init.h"

#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "lan";

static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_glue;

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
        ESP_LOGI(TAG, "link down");
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
    ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
}

void lan_init(void)
{
    /* Initialize TCP/IP stack and default event loop. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create esp-netif for Ethernet (DHCP client by default). */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);

    /* MAC + PHY config using ESP-IDF defaults for the ESP32-P4 dev board.
     * See esp_eth_mac_esp.h — RMII, MDC=31, MDIO=52, CLK=50, etc. */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_mac_config_t        mac_cfg  = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t        phy_cfg  = ETH_PHY_DEFAULT_CONFIG();

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    ESP_ERROR_CHECK(mac == NULL ? ESP_FAIL : ESP_OK);

    /* Generic IEEE 802.3 PHY — works with most on-board PHY chips. */
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
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

    /* Start driver — link negotiation and DHCP begin. */
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    ESP_LOGI(TAG, "LAN init done — waiting for link and DHCP");
}
