#include "lan_runtime.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_eth_phy_ip101.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"

#define PWOS_LAN_PHY_ADDR 1
#define PWOS_LAN_PHY_RESET_GPIO 51

typedef struct {
    esp_eth_handle_t eth_handle;
    esp_eth_netif_glue_handle_t glue;
    esp_netif_t *netif;
    portMUX_TYPE lock;
    pwos_lan_runtime_status_t status;
} pwos_lan_runtime_t;

static const char *TAG = "pwos_lan";
static pwos_lan_runtime_t g_lan = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void set_last_error(esp_err_t error)
{
    portENTER_CRITICAL(&g_lan.lock);
    g_lan.status.last_error = (int32_t)error;
    portEXIT_CRITICAL(&g_lan.lock);
}

static void ethernet_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    esp_eth_handle_t handle = NULL;
    uint8_t mac[6] = {0};

    (void)arg;
    (void)event_base;
    if (event_data != NULL) {
        handle = *(esp_eth_handle_t *)event_data;
    }

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        if (handle != NULL && esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac) != ESP_OK) {
            memset(mac, 0, sizeof(mac));
        }
        portENTER_CRITICAL(&g_lan.lock);
        g_lan.status.link_up = 1u;
        memcpy(g_lan.status.mac, mac, sizeof(mac));
        ++g_lan.status.link_up_events;
        portEXIT_CRITICAL(&g_lan.lock);
        ESP_LOGI(TAG,
            "link up mac=%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&g_lan.lock);
        g_lan.status.link_up = 0u;
        g_lan.status.has_ip = 0u;
        g_lan.status.ip[0] = '\0';
        g_lan.status.netmask[0] = '\0';
        g_lan.status.gateway[0] = '\0';
        ++g_lan.status.link_down_events;
        portEXIT_CRITICAL(&g_lan.lock);
        ESP_LOGW(TAG, "link down");
        break;

    case ETHERNET_EVENT_START:
        portENTER_CRITICAL(&g_lan.lock);
        g_lan.status.started = 1u;
        portEXIT_CRITICAL(&g_lan.lock);
        ESP_LOGI(TAG, "driver started");
        break;

    case ETHERNET_EVENT_STOP:
        portENTER_CRITICAL(&g_lan.lock);
        g_lan.status.started = 0u;
        g_lan.status.link_up = 0u;
        g_lan.status.has_ip = 0u;
        portEXIT_CRITICAL(&g_lan.lock);
        ESP_LOGI(TAG, "driver stopped");
        break;

    default:
        break;
    }
}

static void got_ip_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    char hostname[PWOS_LAN_HOSTNAME_CAP];

    (void)arg;
    (void)event_base;
    (void)event_id;
    if (event == NULL) {
        return;
    }

    portENTER_CRITICAL(&g_lan.lock);
    (void)snprintf(g_lan.status.ip, sizeof(g_lan.status.ip), IPSTR,
        IP2STR(&event->ip_info.ip));
    (void)snprintf(g_lan.status.netmask, sizeof(g_lan.status.netmask), IPSTR,
        IP2STR(&event->ip_info.netmask));
    (void)snprintf(g_lan.status.gateway, sizeof(g_lan.status.gateway), IPSTR,
        IP2STR(&event->ip_info.gw));
    g_lan.status.has_ip = 1u;
    ++g_lan.status.got_ip_events;
    memcpy(hostname, g_lan.status.hostname, sizeof(hostname));
    portEXIT_CRITICAL(&g_lan.lock);

    ESP_LOGI(TAG, "got ip " IPSTR ", open http://%s.local/",
        IP2STR(&event->ip_info.ip), hostname);
}

static void sntp_sync_callback(struct timeval *time_value)
{
    uint64_t unix_ms;

    if (time_value == NULL) return;
    unix_ms = (uint64_t)time_value->tv_sec * 1000u +
        (uint64_t)time_value->tv_usec / 1000u;
    portENTER_CRITICAL(&g_lan.lock);
    g_lan.status.wall_clock_valid = time_value->tv_sec >= 1700000000 ? 1u : 0u;
    g_lan.status.last_sync_unix_ms = unix_ms;
    ++g_lan.status.sntp_sync_events;
    portEXIT_CRITICAL(&g_lan.lock);
    ESP_LOGI(TAG, "SNTP synchronized unix_ms=%llu",
        (unsigned long long)unix_ms);
}

static esp_err_t setup_sntp(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t error;

    config.sync_cb = sntp_sync_callback;
    error = esp_netif_sntp_init(&config);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&g_lan.lock);
        g_lan.status.sntp_started = 1u;
        portEXIT_CRITICAL(&g_lan.lock);
    }
    return error;
}

static esp_err_t setup_mdns(void)
{
    esp_err_t error;
    char hostname[PWOS_LAN_HOSTNAME_CAP];

    portENTER_CRITICAL(&g_lan.lock);
    memcpy(hostname, g_lan.status.hostname, sizeof(hostname));
    portEXIT_CRITICAL(&g_lan.lock);

    error = mdns_init();
    if (error != ESP_OK) {
        return error;
    }
    error = mdns_hostname_set(hostname);
    if (error == ESP_OK) {
        error = mdns_instance_name_set("9P-Walkers Coordinator");
    }
    if (error == ESP_OK) {
        error = mdns_service_add(NULL, "_http", "_tcp", 80u, NULL, 0u);
    }
    if (error == ESP_OK) {
        portENTER_CRITICAL(&g_lan.lock);
        g_lan.status.mdns_ready = 1u;
        portEXIT_CRITICAL(&g_lan.lock);
    }
    return error;
}

int pwos_lan_runtime_start(void)
{
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_eth_config_t eth_config;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_err_t error;
    uint8_t eth_mac[6];
    char hostname[PWOS_LAN_HOSTNAME_CAP];

    portENTER_CRITICAL(&g_lan.lock);
    ++g_lan.status.start_attempts;
    if (g_lan.status.initialized != 0u) {
        portEXIT_CRITICAL(&g_lan.lock);
        return 0;
    }
    portEXIT_CRITICAL(&g_lan.lock);

    error = esp_read_mac(eth_mac, ESP_MAC_ETH);
    if (error != ESP_OK) {
        goto fail;
    }
    (void)snprintf(
        hostname,
        sizeof(hostname),
        PWOS_LAN_HOSTNAME_PREFIX "-%02x%02x",
        eth_mac[4],
        eth_mac[5]);
    portENTER_CRITICAL(&g_lan.lock);
    memcpy(g_lan.status.mac, eth_mac, sizeof(eth_mac));
    (void)snprintf(
        g_lan.status.hostname,
        sizeof(g_lan.status.hostname),
        "%s",
        hostname);
    portEXIT_CRITICAL(&g_lan.lock);

    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        goto fail;
    }

    g_lan.netif = esp_netif_new(&netif_config);
    if (g_lan.netif == NULL) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    error = esp_netif_set_hostname(g_lan.netif, hostname);
    if (error != ESP_OK) {
        goto fail;
    }

    /* ESP32-P4 Function EV Board 使用 IP101GRI，默认 PHY 参数并不适用。 */
    phy_config.phy_addr = PWOS_LAN_PHY_ADDR;
    phy_config.reset_gpio_num = PWOS_LAN_PHY_RESET_GPIO;
    mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    phy = esp_eth_phy_new_ip101(&phy_config);
    if (mac == NULL || phy == NULL) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }

    eth_config = (esp_eth_config_t)ETH_DEFAULT_CONFIG(mac, phy);
    error = esp_eth_driver_install(&eth_config, &g_lan.eth_handle);
    if (error != ESP_OK) {
        goto fail;
    }
    /* driver 安装成功后由 driver 持有 MAC/PHY，失败清理不再直接释放。 */
    mac = NULL;
    phy = NULL;

    g_lan.glue = esp_eth_new_netif_glue(g_lan.eth_handle);
    if (g_lan.glue == NULL) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    error = esp_netif_attach(g_lan.netif, g_lan.glue);
    if (error != ESP_OK) {
        goto fail;
    }
    error = esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, ethernet_event_handler, NULL);
    if (error != ESP_OK) {
        goto fail;
    }
    error = esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, NULL);
    if (error != ESP_OK) {
        (void)esp_event_handler_unregister(
            ETH_EVENT, ESP_EVENT_ANY_ID, ethernet_event_handler);
        goto fail;
    }
    error = esp_eth_start(g_lan.eth_handle);
    if (error != ESP_OK) {
        (void)esp_event_handler_unregister(
            IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler);
        (void)esp_event_handler_unregister(
            ETH_EVENT, ESP_EVENT_ANY_ID, ethernet_event_handler);
        goto fail;
    }

    portENTER_CRITICAL(&g_lan.lock);
    g_lan.status.initialized = 1u;
    g_lan.status.started = 1u;
    g_lan.status.last_error = 0;
    portEXIT_CRITICAL(&g_lan.lock);

    error = setup_mdns();
    if (error != ESP_OK) {
        set_last_error(error);
        ESP_LOGW(TAG, "mDNS unavailable: %s", esp_err_to_name(error));
    }
    error = setup_sntp();
    if (error != ESP_OK) {
        set_last_error(error);
        ESP_LOGW(TAG, "SNTP unavailable: %s", esp_err_to_name(error));
    }
    ESP_LOGI(TAG, "LAN initialized, waiting for link and DHCP");
    return 0;

fail:
    if (g_lan.glue != NULL) {
        esp_eth_del_netif_glue(g_lan.glue);
        g_lan.glue = NULL;
    }
    if (g_lan.eth_handle != NULL) {
        esp_eth_mac_t *installed_mac = NULL;
        esp_eth_phy_t *installed_phy = NULL;

        (void)esp_eth_get_mac_instance(g_lan.eth_handle, &installed_mac);
        (void)esp_eth_get_phy_instance(g_lan.eth_handle, &installed_phy);
        (void)esp_eth_driver_uninstall(g_lan.eth_handle);
        if (installed_mac != NULL) {
            installed_mac->del(installed_mac);
        }
        if (installed_phy != NULL) {
            installed_phy->del(installed_phy);
        }
        g_lan.eth_handle = NULL;
    }
    if (mac != NULL) {
        mac->del(mac);
    }
    if (phy != NULL) {
        phy->del(phy);
    }
    if (g_lan.netif != NULL) {
        esp_netif_destroy(g_lan.netif);
        g_lan.netif = NULL;
    }
    portENTER_CRITICAL(&g_lan.lock);
    ++g_lan.status.start_failures;
    g_lan.status.last_error = (int32_t)error;
    portEXIT_CRITICAL(&g_lan.lock);
    ESP_LOGE(TAG, "LAN init failed: %s", esp_err_to_name(error));
    return (int)error;
}

void pwos_lan_runtime_get_status(pwos_lan_runtime_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    portENTER_CRITICAL(&g_lan.lock);
    *out_status = g_lan.status;
    portEXIT_CRITICAL(&g_lan.lock);
}

int pwos_lan_runtime_publish_host_rpc(
    const uint32_t uid[3],
    uint32_t epoch,
    uint16_t priority,
    uint16_t port)
{
    char uid_text[25];
    char epoch_text[11];
    char priority_text[6];
    mdns_txt_item_t txt[4];
    esp_err_t error;

    if (uid == NULL || port == 0u) {
        return (int)ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(uid_text, sizeof(uid_text), "%08lx%08lx%08lx",
        (unsigned long)uid[0], (unsigned long)uid[1], (unsigned long)uid[2]);
    (void)snprintf(epoch_text, sizeof(epoch_text), "%lu", (unsigned long)epoch);
    (void)snprintf(priority_text, sizeof(priority_text), "%u", priority);
    txt[0] = (mdns_txt_item_t){"v", "1"};
    txt[1] = (mdns_txt_item_t){"uid", uid_text};
    txt[2] = (mdns_txt_item_t){"epoch", epoch_text};
    txt[3] = (mdns_txt_item_t){"priority", priority_text};
    error = mdns_service_add(
        NULL, "_pwos", "_tcp", port, txt, sizeof(txt) / sizeof(txt[0]));
    if (error == ESP_OK) {
        portENTER_CRITICAL(&g_lan.lock);
        g_lan.status.host_rpc_mdns_ready = 1u;
        portEXIT_CRITICAL(&g_lan.lock);
    }
    return (int)error;
}
