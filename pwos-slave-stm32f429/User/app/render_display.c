#include "render_display.h"

#include <string.h>

#include "FreeRTOS.h"
#include "gfx.h"
#include "pwos_smallpt.h"
#include "semphr.h"

/* 两个 LTDC 帧缓冲之后的 SDRAM 空间用作低分辨率合成画布。 */
#define PWOS_RENDER_CANVAS_ADDR (GFX_FB_B_ADDR + GFX_FB_BYTES)

static volatile uint16_t *const g_canvas =
    (volatile uint16_t *)PWOS_RENDER_CANVAS_ADDR;
static StaticSemaphore_t g_mutex_storage;
static SemaphoreHandle_t g_mutex;
static pwos_render_display_status_t g_status;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

int pwos_render_display_init(void)
{
    uint32_t i;

    memset(&g_status, 0, sizeof(g_status));
    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_storage);
    if (g_mutex == NULL) return -1;
    for (i = 0u; i < PWOS_RENDER_CANVAS_WIDTH * PWOS_RENDER_CANVAS_HEIGHT; ++i) {
        g_canvas[i] = 0u;
    }
    g_status.initialized = 1u;
    g_status.dirty = 1u;
    return 0;
}

int pwos_render_display_apply_tile(const uint8_t *data, uint16_t len)
{
    uint8_t x;
    uint8_t y;
    uint8_t width;
    uint8_t height;
    uint16_t expected;
    uint32_t row;
    uint32_t col;

    if (g_status.initialized == 0u) return -2;
    if (data == NULL || len < PWOS_RENDER_RESULT_HEADER_LEN ||
        data[0] != PWOS_RENDER_PROTOCOL_VERSION ||
        data[1] != PWOS_RENDER_SCENE_WHITTED ||
        data[6] != PWOS_RENDER_CANVAS_WIDTH ||
        data[7] != PWOS_RENDER_CANVAS_HEIGHT ||
        data[10] != PWOS_RENDER_FORMAT_RGB565_LE) {
        ++g_status.rejected_tiles;
        return -1;
    }
    x = data[2];
    y = data[3];
    width = data[4];
    height = data[5];
    expected = (uint16_t)(PWOS_RENDER_RESULT_HEADER_LEN +
        (uint16_t)width * height * 2u);
    if (width == 0u || height == 0u || expected != len ||
        (uint16_t)x + width > PWOS_RENDER_CANVAS_WIDTH ||
        (uint16_t)y + height > PWOS_RENDER_CANVAS_HEIGHT) {
        ++g_status.rejected_tiles;
        return -1;
    }

    (void)xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_status.frame_id = read_le16(data + 8u);
    for (row = 0u; row < height; ++row) {
        for (col = 0u; col < width; ++col) {
            uint32_t source = PWOS_RENDER_RESULT_HEADER_LEN +
                (row * width + col) * 2u;
            g_canvas[((uint32_t)y + row) * PWOS_RENDER_CANVAS_WIDTH + x + col] =
                read_le16(data + source);
        }
    }
    ++g_status.tiles_received;
    g_status.dirty = 1u;
    (void)xSemaphoreGive(g_mutex);
    return 0;
}

int pwos_render_display_step(void)
{
    uint16_t *framebuffer;
    uint32_t y;
    uint32_t x;

    if (g_status.initialized == 0u || g_status.dirty == 0u) return 0;
    framebuffer = gfx_framebuffer();
    (void)xSemaphoreTake(g_mutex, portMAX_DELAY);
    for (y = 0u; y < PWOS_RENDER_CANVAS_HEIGHT; ++y) {
        uint32_t dst_y = y * 2u;

        for (x = 0u; x < PWOS_RENDER_CANVAS_WIDTH; ++x) {
            uint16_t pixel = g_canvas[y * PWOS_RENDER_CANVAS_WIDTH + x];
            uint32_t dst_x = x * 2u;

            framebuffer[dst_y * GFX_LCD_WIDTH + dst_x] = pixel;
            framebuffer[dst_y * GFX_LCD_WIDTH + dst_x + 1u] = pixel;
            framebuffer[(dst_y + 1u) * GFX_LCD_WIDTH + dst_x] = pixel;
            framebuffer[(dst_y + 1u) * GFX_LCD_WIDTH + dst_x + 1u] = pixel;
        }
    }
    g_status.dirty = 0u;
    ++g_status.frames_presented;
    (void)xSemaphoreGive(g_mutex);
    return 1;
}

void pwos_render_display_get_status(pwos_render_display_status_t *out_status)
{
    if (out_status == NULL) return;
    if (g_mutex == NULL) {
        *out_status = g_status;
        return;
    }
    (void)xSemaphoreTake(g_mutex, portMAX_DELAY);
    *out_status = g_status;
    (void)xSemaphoreGive(g_mutex);
}
