#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_shell_runtime.h"
#include "host_rpc_runtime.h"
#include "http_server.h"
#include "lan_runtime.h"
#include "pwos_coordinator_runtime.h"
#include "sdkconfig.h"

static const char *TAG = "pwos_main";

/* 打印系统初始信息，方便串口日志确认当前烧录的是 coordinator 固件。 */
static void print_chip_banner(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0u;

    // 获取 chip info
    esp_chip_info(&chip_info);
    printf("pwos master booting on %s\n", CONFIG_IDF_TARGET);
    printf("cpu cores: %d, revision: %d\n", chip_info.cores, chip_info.revision);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("flash size: %" PRIu32 " MB\n", flash_size / (uint32_t)(1024u * 1024u));
    }

    printf("free heap: %u bytes\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void app_main(void)
{
    pwos_command_service_config_t command_config;
    int rc;

    /* stdout 无缓冲，确保启动日志立即出现在 USB Serial/JTAG console。 */
    setvbuf(stdout, NULL, _IONBF, 0);

    print_chip_banner();

    if (pwos_coordinator_runtime_start_default() != 0) {
        puts("fatal: pwos coordinator runtime init failed");
        return;
    }

    /* 网络和 WebShell 是可降级服务，失败时不能影响串口协调器继续工作。 */
    rc = pwos_lan_runtime_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "LAN service unavailable rc=%d", rc);
    } else {
        rc = pwos_host_rpc_runtime_start();
        if (rc != 0) {
            ESP_LOGE(TAG, "host RPC service unavailable rc=%d", rc);
        }
    }

    rc = pwos_host_shell_runtime_build_config(&command_config);
    if (rc == 0) {
        rc = pwos_http_server_start(&command_config);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "WebShell service unavailable rc=%d", rc);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
