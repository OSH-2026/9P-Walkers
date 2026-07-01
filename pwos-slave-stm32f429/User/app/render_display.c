#include "render_display.h"

#include <string.h>

#include "FreeRTOS.h"
#include "gfx.h"
#include "pwos_smallpt.h"
#include "semphr.h"

#define PWOS_RENDER_CANVAS_ADDR (GFX_FB_B_ADDR + GFX_FB_BYTES)

static volatile uint16_t *const g_canvas =
    (volatile uint16_t *)PWOS_RENDER_CANVAS_ADDR;
static StaticSemaphore_t g_mutex_storage;
static SemaphoreHandle_t g_mutex;
static pwos_render_display_status_t g_status;

static uint16_t read_le16(const uint8_t *d)
{
    return (uint16_t)(d[0] | ((uint16_t)d[1] << 8));
}

int pwos_render_display_init(void)
{
    memset(&g_status, 0, sizeof(g_status));
    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_storage);
    if (g_mutex == NULL) return -1;
    /* 画布初始化为深灰，区分 "未渲染" 和 "渲染后黑色" */
    volatile uint16_t *c = g_canvas;
    uint32_t i;
    for (i = 0u; i < PWOS_RENDER_CANVAS_WIDTH * PWOS_RENDER_CANVAS_HEIGHT; ++i)
        c[i] = 0x4208u; /* RGB565 深灰 */
    g_status.initialized = 1u;
    g_status.dirty = 1u;
    return 0;
}

int pwos_render_display_apply_tile(const uint8_t *data, uint16_t len)
{
    uint16_t x, y, w, h;
    uint16_t expected;
    uint32_t row, col;

    if (g_status.initialized == 0u) return -2;
    if (data == NULL || len < PWOS_RENDER_RESULT_HEADER_LEN ||
        data[0] != PWOS_RENDER_PROTOCOL_VERSION ||
        data[1] != PWOS_RENDER_SCENE_WHITTED ||
        read_le16(data + 8) != PWOS_RENDER_CANVAS_WIDTH ||
        read_le16(data + 10) != PWOS_RENDER_CANVAS_HEIGHT ||
        data[14] != PWOS_RENDER_FORMAT_RGB565_LE) {
        ++g_status.rejected_tiles;
        return -1;
    }
    x = read_le16(data + 2);
    y = read_le16(data + 4);
    w = data[6];
    h = data[7];
    expected = (uint16_t)(PWOS_RENDER_RESULT_HEADER_LEN + (uint16_t)w * h * 2u);
    if (w == 0u || h == 0u || expected != len ||
        (uint16_t)x + w > PWOS_RENDER_CANVAS_WIDTH ||
        (uint16_t)y + h > PWOS_RENDER_CANVAS_HEIGHT) {
        ++g_status.rejected_tiles;
        return -1;
    }

    (void)xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_status.frame_id = read_le16(data + 12);
    for (row = 0u; row < h; ++row) {
        for (col = 0u; col < w; ++col) {
            uint32_t src = PWOS_RENDER_RESULT_HEADER_LEN + (row * w + col) * 2u;
            g_canvas[((uint32_t)y + row) * PWOS_RENDER_CANVAS_WIDTH + x + col] =
                read_le16(data + src);
        }
    }
    ++g_status.tiles_received;
    g_status.dirty = 1u;
    (void)xSemaphoreGive(g_mutex);
    return 0;
}

int pwos_render_display_step(void)
{
    uint16_t *fb;

    if (g_status.initialized == 0u || g_status.dirty == 0u) return 0;
    fb = gfx_framebuffer();
    (void)xSemaphoreTake(g_mutex, portMAX_DELAY);
    memcpy(fb, (const void *)g_canvas, GFX_FB_BYTES);
    g_status.dirty = 0u;
    ++g_status.frames_presented;
    (void)xSemaphoreGive(g_mutex);
    return 1;
}

void pwos_render_display_get_status(pwos_render_display_status_t *out_status)
{
    if (out_status == NULL) return;
    if (g_mutex == NULL) { *out_status = g_status; return; }
    (void)xSemaphoreTake(g_mutex, portMAX_DELAY);
    *out_status = g_status;
    (void)xSemaphoreGive(g_mutex);
}
