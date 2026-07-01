#include "lan_runtime.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

typedef struct {
    portMUX_TYPE lock;
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    pwos_lan_runtime_status_t status;
} wifi_runtime_t;

static const char *TAG = "pwos_wifi";
static wifi_runtime_t g_wifi = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void set_last_error(esp_err_t error)
{
    portENTER_CRITICAL(&g_wifi.lock);
    g_wifi.status.last_error = (int32_t)error;
    portEXIT_CRITICAL(&g_wifi.lock);
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    if (event_id == WIFI_EVENT_STA_START) {
        esp_err_t error;

        portENTER_CRITICAL(&g_wifi.lock);
        g_wifi.status.started = 1u;
        portEXIT_CRITICAL(&g_wifi.lock);
        error = esp_wifi_connect();
        if (error != ESP_OK) set_last_error(error);
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        portENTER_CRITICAL(&g_wifi.lock);
        g_wifi.status.link_up = 1u;
        ++g_wifi.status.link_up_events;
        portEXIT_CRITICAL(&g_wifi.lock);
        ESP_LOGI(TAG, "connected ssid=%s", CONFIG_PWOS_WIFI_SSID);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_err_t error;

        portENTER_CRITICAL(&g_wifi.lock);
        g_wifi.status.link_up = 0u;
        g_wifi.status.has_ip = 0u;
        g_wifi.status.ip[0] = '\0';
        ++g_wifi.status.link_down_events;
        portEXIT_CRITICAL(&g_wifi.lock);
        error = esp_wifi_connect();
        if (error != ESP_OK) set_last_error(error);
        ESP_LOGW(TAG, "disconnected, reconnecting");
    }
}

static void ip_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    char hostname[PWOS_LAN_HOSTNAME_CAP];

    (void)arg;
    (void)event_base;
    if (event_id != IP_EVENT_STA_GOT_IP || event == NULL) return;
    portENTER_CRITICAL(&g_wifi.lock);
    (void)snprintf(g_wifi.status.ip, sizeof(g_wifi.status.ip), IPSTR,
        IP2STR(&event->ip_info.ip));
    (void)snprintf(g_wifi.status.netmask, sizeof(g_wifi.status.netmask), IPSTR,
        IP2STR(&event->ip_info.netmask));
    (void)snprintf(g_wifi.status.gateway, sizeof(g_wifi.status.gateway), IPSTR,
        IP2STR(&event->ip_info.gw));
    g_wifi.status.has_ip = 1u;
    ++g_wifi.status.got_ip_events;
    memcpy(hostname, g_wifi.status.hostname, sizeof(hostname));
    portEXIT_CRITICAL(&g_wifi.lock);
    ESP_LOGI(TAG, "got ip " IPSTR " host=%s.local",
        IP2STR(&event->ip_info.ip), hostname);
}

static void sntp_sync_callback(struct timeval *time_value)
{
    uint64_t unix_ms;

    if (time_value == NULL) return;
    unix_ms = (uint64_t)time_value->tv_sec * 1000u +
        (uint64_t)time_value->tv_usec / 1000u;
    portENTER_CRITICAL(&g_wifi.lock);
    g_wifi.status.wall_clock_valid = time_value->tv_sec >= 1700000000 ? 1u : 0u;
    g_wifi.status.last_sync_unix_ms = unix_ms;
    ++g_wifi.status.sntp_sync_events;
    portEXIT_CRITICAL(&g_wifi.lock);
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
        portENTER_CRITICAL(&g_wifi.lock);
        g_wifi.status.sntp_started = 1u;
        portEXIT_CRITICAL(&g_wifi.lock);
    }
    return error;
}

static esp_err_t init_nvs(void)
{
    esp_err_t error = nvs_flash_init();

    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) error = nvs_flash_init();
    }
    return error;
}

static esp_err_t setup_mdns(const char *hostname)
{
    esp_err_t error = mdns_init();

    if (error == ESP_OK) error = mdns_hostname_set(hostname);
    if (error == ESP_OK) error = mdns_instance_name_set("9P-Walkers S3 Coordinator");
    if (error == ESP_OK) {
        portENTER_CRITICAL(&g_wifi.lock);
        g_wifi.status.mdns_ready = 1u;
        portEXIT_CRITICAL(&g_wifi.lock);
    }
    return error;
}

int pwos_lan_runtime_start(void)
{
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config;
    uint8_t mac[6];
    char hostname[PWOS_LAN_HOSTNAME_CAP];
    esp_err_t error;

    portENTER_CRITICAL(&g_wifi.lock);
    ++g_wifi.status.start_attempts;
    if (g_wifi.status.initialized != 0u) {
        portEXIT_CRITICAL(&g_wifi.lock);
        return 0;
    }
    portEXIT_CRITICAL(&g_wifi.lock);

    error = init_nvs();
    if (error != ESP_OK) goto fail;
    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) goto fail;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) goto fail;
    g_wifi.netif = esp_netif_create_default_wifi_sta();
    if (g_wifi.netif == NULL) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    error = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (error != ESP_OK) goto fail;
    (void)snprintf(hostname, sizeof(hostname), PWOS_LAN_HOSTNAME_PREFIX "-%02x%02x",
        mac[4], mac[5]);
    error = esp_netif_set_hostname(g_wifi.netif, hostname);
    if (error != ESP_OK) goto fail;
    error = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
        &g_wifi.wifi_handler);
    if (error != ESP_OK) goto fail;
    error = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL,
        &g_wifi.ip_handler);
    if (error != ESP_OK) goto fail;
    error = esp_wifi_init(&init_config);
    if (error != ESP_OK) goto fail;
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error != ESP_OK) goto fail;
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error != ESP_OK) goto fail;

    memset(&wifi_config, 0, sizeof(wifi_config));
    (void)snprintf((char *)wifi_config.sta.ssid,
        sizeof(wifi_config.sta.ssid), "%s", CONFIG_PWOS_WIFI_SSID);
    (void)snprintf((char *)wifi_config.sta.password,
        sizeof(wifi_config.sta.password), "%s", CONFIG_PWOS_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (error != ESP_OK) goto fail;
    error = setup_mdns(hostname);
    if (error != ESP_OK) goto fail;

    error = setup_sntp();
    if (error != ESP_OK) {
        set_last_error(error);
        ESP_LOGW(TAG, "SNTP unavailable: %s", esp_err_to_name(error));
    }

    portENTER_CRITICAL(&g_wifi.lock);
    memcpy(g_wifi.status.mac, mac, sizeof(mac));
    (void)snprintf(g_wifi.status.hostname,
        sizeof(g_wifi.status.hostname), "%s", hostname);
    g_wifi.status.initialized = 1u;
    g_wifi.status.last_error = 0;
    portEXIT_CRITICAL(&g_wifi.lock);
    error = esp_wifi_start();
    if (error != ESP_OK) goto fail;
    ESP_LOGI(TAG, "starting ssid=%s host=%s", CONFIG_PWOS_WIFI_SSID, hostname);
    return 0;

fail:
    portENTER_CRITICAL(&g_wifi.lock);
    ++g_wifi.status.start_failures;
    g_wifi.status.last_error = (int32_t)error;
    portEXIT_CRITICAL(&g_wifi.lock);
    ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(error));
    return (int)error;
}

void pwos_lan_runtime_get_status(pwos_lan_runtime_status_t *out_status)
{
    if (out_status == NULL) return;
    portENTER_CRITICAL(&g_wifi.lock);
    *out_status = g_wifi.status;
    portEXIT_CRITICAL(&g_wifi.lock);
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

    if (uid == NULL || port == 0u) return (int)ESP_ERR_INVALID_ARG;
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
        portENTER_CRITICAL(&g_wifi.lock);
        g_wifi.status.host_rpc_mdns_ready = 1u;
        portEXIT_CRITICAL(&g_wifi.lock);
    }
    return (int)error;
}
