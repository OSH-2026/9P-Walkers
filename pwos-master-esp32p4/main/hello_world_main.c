#include <inttypes.h>
#include <stdio.h>

#include "cluster_config.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "http_server.h"
#include "lua_port.h"
#include "mesh_host_runtime.h"
#include "sdkconfig.h"
#include "shell.h"
#include "wifi_softap.h"

// 打印系统初始信息
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
    setvbuf(stdin,  NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    print_chip_banner();

    /*
     * 启动真正的主机侧 mesh runtime。
     *
     * 该入口会一次性完成：
     * 1. 初始化共享 mesh cluster + cluster_vfs 桥接层；
     * 2. 初始化默认 UART raw transport；
     * 3. 创建后台轮询任务，持续处理 REGISTER / LINK_STATE / MINI9P mesh 帧；
     * 4. 在 REGISTER 到来时自动把 UID 同步到 VFS，并为该节点创建真实的
     *    mesh-backed m9p_client。
     */
    if (mesh_host_runtime_start_default_task() != 0) {
        puts("fatal: mesh host runtime init failed");
        return;
    }

    if (!pw_lua_init()) {
        puts("fatal: Lua init failed");
        return;
    }

    /* Bring up WiFi SoftAP so browsers can reach the web server.
     * SSID: 9P-Walkers  Password: pwos1234  IP: 192.168.4.1 */
    wifi_softap_init();

    /* Start HTTP + WebSocket server (port 80). */
    web_server_start();

    shell_run_boot_demo();
    shell_start();
}
