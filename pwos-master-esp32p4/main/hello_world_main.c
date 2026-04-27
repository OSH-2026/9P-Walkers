#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lua_port.h"
#include "sdkconfig.h"
#include "shell.h"

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
    // 禁用 stdin 和 stdout 的缓冲，以确保输出立即显示
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    print_chip_banner();

    // 初始化 Lua 环境
    if (!pw_lua_init()) {
        puts("fatal: Lua init failed");
        return;
    }

    // 启动 demo shell
    shell_run_boot_demo();
    shell_start();
}
