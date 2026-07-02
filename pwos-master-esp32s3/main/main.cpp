#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_rpc_runtime.h"
#include "dist_inference_service.h"
#include "inference_runtime.h"
#include "lan_runtime.h"
#include "pwos_coordinator_runtime.h"
#include "sdkconfig.h"

#define PWOS_BOOT_TIMEOUT_MS 30000u  /* 整体启动超时 */
#define WDT_FEED_INTERVAL_MS 3000u   /* 看门狗喂狗间隔（须 < TWDT 超时 5s） */
#define HEALTH_CHECK_CYCLES  10u     /* 每 10 次喂狗 = 30s 执行一次健康检查 */
/*
 * app_main 栈大小由 sdkconfig 中 CONFIG_ESP_MAIN_TASK_STACK_SIZE 控制。
 * 当前 sdkconfig.defaults 已设为 8192（默认 3584 不够容纳 WiFi/协调器初始化）。
 */

static const char *TAG = "pwos_s3";

/* ------------------------------------------------------------------ */
/* 启动阶段枚举，用于追踪当前进度，方便定位挂死阶段                    */
/* ------------------------------------------------------------------ */
typedef enum {
    BOOT_PHASE_START = 0,
    BOOT_PHASE_COORDINATOR,
    BOOT_PHASE_LAN,
    BOOT_PHASE_HOST_RPC,
    BOOT_PHASE_DIST_INFERENCE,
    BOOT_PHASE_INFERENCE,
    BOOT_PHASE_DONE,
} boot_phase_t;

static boot_phase_t g_boot_phase = BOOT_PHASE_START;
static int64_t      g_boot_start_us = 0;

/* ------------------------------------------------------------------ */
/* 打印启动横幅（不泄露敏感内存信息）                                   */
/* ------------------------------------------------------------------ */
static void print_banner(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0u;

    esp_chip_info(&chip);
    (void)esp_flash_get_size(NULL, &flash_size);

    /* 仅输出芯片型号和核心数；内存细节降级为 DEBUG 日志 */
    ESP_LOGI(TAG, "boot target=%s cores=%d flash=%" PRIu32 "MB",
             CONFIG_IDF_TARGET, chip.cores,
             flash_size / (1024u * 1024u));

    ESP_LOGD(TAG, "psram total=%u free=%u",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ------------------------------------------------------------------ */
/* 启动阶段推进，同时检查全局超时                                      */
/* ------------------------------------------------------------------ */
static void advance_phase(boot_phase_t phase)
{
    int64_t now = esp_timer_get_time();

    g_boot_phase = phase;
    if (g_boot_start_us == 0) g_boot_start_us = now;

    if ((now - g_boot_start_us) / 1000 > PWOS_BOOT_TIMEOUT_MS) {
        ESP_LOGE(TAG, "启动超时 phase=%d elapsed=%lld ms，重启系统",
                 (int)phase, (long long)((now - g_boot_start_us) / 1000));
        esp_restart();
    }
}

/* ------------------------------------------------------------------ */
/* app_main                                                           */
/* ------------------------------------------------------------------ */
extern "C" void app_main(void)
{
    int rc;

    /* 0. 基础环境 */
    setvbuf(stdout, NULL, _IONBF, 0);
    g_boot_start_us = esp_timer_get_time();
    print_banner();

    /* 1. 协调器运行时（依赖项，失败不阻塞但应告警） */
    advance_phase(BOOT_PHASE_COORDINATOR);
    rc = pwos_coordinator_runtime_start_default();
    if (rc != 0) {
        ESP_LOGW(TAG, "coordinator 不可用 rc=%d（系统将以降级模式运行）", rc);
    }

    /* 2. WiFi / LAN */
    advance_phase(BOOT_PHASE_LAN);
    rc = pwos_lan_runtime_start();
    if (rc != 0) {
        ESP_LOGW(TAG, "WiFi 不可用 rc=%d（9P 网络功能将不可用）", rc);
    } else {
        /* 3. 仅 LAN 成功时才启动 Host RPC */
        advance_phase(BOOT_PHASE_HOST_RPC);
        rc = pwos_host_rpc_runtime_start();
        if (rc != 0) {
            ESP_LOGW(TAG, "host RPC 不可用 rc=%d", rc);
        }
    }

    /* 4. 分布式推理服务（先于推理引擎初始化，仅注册 9P 文件树，不加载模型） */
    advance_phase(BOOT_PHASE_DIST_INFERENCE);
    rc = pwos_dist_inference_service_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "分布式推理服务初始化失败 rc=%d", rc);
        /* 非致命：仍可继续启动推理引擎 */
    }

    /* 5. 推理引擎（核心功能，失败则重启） */
    advance_phase(BOOT_PHASE_INFERENCE);
    rc = pwos_inference_runtime_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "推理引擎启动失败 rc=%d，5 秒后重启", rc);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* 6. 启动完成，注册任务看门狗 */
    advance_phase(BOOT_PHASE_DONE);
    ESP_LOGI(TAG, "系统就绪 phase=%d elapsed=%lld ms",
             (int)g_boot_phase,
             (long long)((esp_timer_get_time() - g_boot_start_us) / 1000));

    /* 将当前任务 (app_main) 注册到 TWDT，超时时间继承 sdkconfig 的 5 秒 */
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "TWDT 已订阅 app_main，喂狗间隔=%u ms", WDT_FEED_INTERVAL_MS);

    /* 7. 空闲循环：喂狗 + 周期性自检 */
    uint32_t cycle = 0u;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WDT_FEED_INTERVAL_MS));

        /* ── 喂狗（每次循环必做，确保 TWDT 不触发）── */
        esp_task_wdt_reset();

        /* ── 健康检查（每 HEALTH_CHECK_CYCLES 次 = 约 30 秒）── */
        cycle++;
        if (cycle >= HEALTH_CHECK_CYCLES) {
            cycle = 0u;

            size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGD(TAG, "heartbeat psram_free=%.1f KB",
                     (double)free_psram / 1024.0);

            /* 若 PSRAM 严重不足（< 128 KB），主动重启以防 OOM 连锁故障 */
            if (free_psram < 128u * 1024u) {
                ESP_LOGE(TAG, "PSRAM 严重不足 (%.1f KB)，主动重启",
                         (double)free_psram / 1024.0);
                vTaskDelay(pdMS_TO_TICKS(3000));
                esp_restart();
            }
        }
    }
}
