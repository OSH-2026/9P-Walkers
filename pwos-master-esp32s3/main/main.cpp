#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_rpc_runtime.h"
#include "dist_inference_service.h"
#include "inference_runtime.h"
#include "lan_runtime.h"
#include "pwos_coordinator_runtime.h"
#include "sdkconfig.h"

static const char *TAG = "pwos_s3";

static void print_banner(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0u;

    esp_chip_info(&chip);
    (void)esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG,
        "boot target=%s cores=%d flash=%" PRIu32 "MB psram=%u free=%u",
        CONFIG_IDF_TARGET,
        chip.cores,
        flash_size / (1024u * 1024u),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

extern "C" void app_main(void)
{
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    print_banner();

    rc = pwos_coordinator_runtime_start_default();
    if (rc != 0) {
        ESP_LOGE(TAG, "coordinator unavailable rc=%d", rc);
    }
    rc = pwos_lan_runtime_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "WiFi unavailable rc=%d", rc);
    } else {
        rc = pwos_host_rpc_runtime_start();
        if (rc != 0) ESP_LOGE(TAG, "host RPC unavailable rc=%d", rc);
    }
    (void)pwos_dist_inference_service_init();
    rc = pwos_inference_runtime_start();
    if (rc != 0) ESP_LOGE(TAG, "inference unavailable rc=%d", rc);

    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
