#include <inttypes.h>
#include <stdio.h>

#include "cluster_config.h"
#include "node_connector.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "http_server.h"
#include "lua_port.h"
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
     * 启动主机侧 mesh cluster + VFS 桥接层。
     * 这里不再预注册静态 mcu1，而是等待后续 mesh 控制面把“节点发现/离线”
     * 事件送入 cluster_config_on_node_discovered()/on_node_departed()。
     */
    if (cluster_config_init_mesh_host() != 0) {
        puts("fatal: mesh host cluster/vfs init failed");
        return;
    }

    /*
     * 注册静态直连从机（STM32F411，UART1 115200，TX=17 RX=18）。
     * 若从机未连接，attach 会失败但不中断启动；
     * 连接好后在 Lua shell 里执行 vfs.attach("mcu1") 重试。
     */
    node_connector_init_static_slave();

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
