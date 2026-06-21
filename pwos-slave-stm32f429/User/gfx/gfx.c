/**
 * @file gfx.c
 * @brief RGB565 framebuffer graphics primitives for the STM32F429I-DISCO.
 *
 * The framebuffer lives in SDRAM at 0xD0000000 (LTDC layer 1). Drawing is
 * plain CPU writes (no DMA2D dependency for the core primitives); gfx_blit
 * variants could be accelerated later if needed.
 */
#include "gfx.h"
#include "bsp_sdram.h"
#include <string.h>

static volatile uint16_t *g_fb = (volatile uint16_t *)BSP_SDRAM_BASE_ADDR;

void gfx_init(void)
{
    g_fb = (volatile uint16_t *)BSP_SDRAM_BASE_ADDR;
    gfx_clear(GFX_BLACK);
}

uint16_t *gfx_framebuffer(void)
{
    return (uint16_t *)g_fb;
}

void gfx_clear(gfx_color_t color)
{
    /* Word-fill the framebuffer. We treat it as uint32_t to halve the loop
     * count: RGB565 pairs into 32 bits naturally on little-endian. */
    uint32_t packed = ((uint32_t)color << 16) | (uint32_t)color;
    volatile uint32_t *p32 = (volatile uint32_t *)g_fb;
    for (int i = 0; i < GFX_FB_PIXELS / 2; i++) {
        p32[i] = packed;
    }
}

static inline void put_pixel_unchecked(int x, int y, gfx_color_t color)
{
    g_fb[y * GFX_LCD_WIDTH + x] = color;
}

void gfx_put_pixel(int x, int y, gfx_color_t color)
{
    if ((unsigned)x >= GFX_LCD_WIDTH || (unsigned)y >= GFX_LCD_HEIGHT) return;
    put_pixel_unchecked(x, y, color);
}

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color)
{
    if (w <= 0 || h <= 0) return;
    /* Clip to screen bounds. */
    int x0 = x, y0 = y;
    int x1 = x + w - 1, y1 = y + h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= GFX_LCD_WIDTH)  x1 = GFX_LCD_WIDTH  - 1;
    if (y1 >= GFX_LCD_HEIGHT) y1 = GFX_LCD_HEIGHT - 1;
    if (x0 > x1 || y0 > y1)   return;

    for (int yy = y0; yy <= y1; yy++) {
        volatile uint16_t *row = &g_fb[yy * GFX_LCD_WIDTH + x0];
        /* Manual fill so the compiler can use 16-bit stores efficiently. */
        int ww = x1 - x0 + 1;
        for (int xx = 0; xx < ww; xx++) {
            row[xx] = color;
        }
    }
}

void gfx_draw_rect(int x, int y, int w, int h, gfx_color_t color)
{
    if (w <= 0 || h <= 0) return;
    int x1 = x + w - 1, y1 = y + h - 1;
    gfx_draw_line(x,  y,  x1, y,  color); /* top    */
    gfx_draw_line(x,  y1, x1, y1, color); /* bottom */
    gfx_draw_line(x,  y,  x,  y1, color); /* left   */
    gfx_draw_line(x1, y,  x1, y1, color); /* right  */
}

void gfx_draw_line(int x0, int y0, int x1, int y1, gfx_color_t color)
{
    /* Bresenham (handles all octants). */
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        gfx_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_draw_line_thick(int x0, int y0, int x1, int y1, int thickness, gfx_color_t color)
{
    if (thickness <= 1) {
        gfx_draw_line(x0, y0, x1, y1, color);
        return;
    }
    /* Draw the line once to find its bbox, then stamp a small filled box at
     * each point on the line. Cheap and looks fine for axis-aligned cube
     * edges on a 240x320 panel. */
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int half = thickness / 2;

    for (;;) {
        gfx_fill_rect(x0 - half, y0 - half, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, gfx_color_t color)
{
    /* Sort vertices by Y ascending. */
    if (y0 > y1) { int t; t=y0;y0=y1;y1=t; t=x0;x0=x1;x1=t; }
    if (y1 > y2) { int t; t=y1;y1=y2;y2=t; t=x1;x1=x2;x2=t; }
    if (y0 > y1) { int t; t=y0;y0=y1;y1=t; t=x0;x0=x1;x1=t; }

    int total_h = y2 - y0;
    if (total_h <= 0) {
        /* Degenerate: fill the 3 points as a single line. */
        gfx_draw_line(x0, y0, x1, y1, color);
        gfx_draw_line(x1, y1, x2, y2, color);
        gfx_draw_line(x2, y2, x0, y0, color);
        return;
    }

    /* Walk each scanline, interpolate both edges, fill the span. */
    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= GFX_LCD_HEIGHT) continue;
        bool second_half = (y > y1) || (y1 == y0);
        int segment_h = second_half ? (y2 - y1) : (y1 - y0);
        if (segment_h == 0) segment_h = 1;
        /* Left edge: v0 -> v2 (whole triangle). */
        int alpha_a = (y - y0) * 256 / total_h;
        int xa = x0 + ((x2 - x0) * alpha_a) / 256;
        /* Right edge: v0 -> v1 then v1 -> v2. */
        int xb;
        if (second_half) {
            int alpha_b = (y - y1) * 256 / segment_h;
            xb = x1 + ((x2 - x1) * alpha_b) / 256;
        } else {
            int alpha_b = (y - y0) * 256 / segment_h;
            xb = x0 + ((x1 - x0) * alpha_b) / 256;
        }
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        if (xa < 0) xa = 0;
        if (xb >= GFX_LCD_WIDTH) xb = GFX_LCD_WIDTH - 1;
        volatile uint16_t *row = &g_fb[y * GFX_LCD_WIDTH + xa];
        for (int x = xa; x <= xb; x++) {
            *row++ = color;
        }
    }
}

void gfx_blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if ((unsigned)x >= GFX_LCD_WIDTH || (unsigned)y >= GFX_LCD_HEIGHT) return;
    if (a == 0) return;
    if (a == 255) {
        put_pixel_unchecked(x, y, gfx_color_rgb565(r, g, b));
        return;
    }
    /* Source premultiplied by alpha, destination by (255 - alpha), in RGB565
     * space. Convert dst to 8-bit, blend, re-quantize. */
    uint16_t dst = g_fb[y * GFX_LCD_WIDTH + x];
    uint8_t dr = (dst >> 11) & 0x1F; dr = (dr << 3) | (dr >> 2);
    uint8_t dg = (dst >>  5) & 0x3F; dg = (dg << 2) | (dg >> 4);
    uint8_t db = (dst      ) & 0x1F; db = (db << 3) | (db >> 2);
    uint16_t ia = 255 - a;
    uint8_t or_ = (uint8_t)((r * a + dr * ia) / 255);
    uint8_t og_ = (uint8_t)((g * a + dg * ia) / 255);
    uint8_t ob_ = (uint8_t)((b * a + db * ia) / 255);
    put_pixel_unchecked(x, y, gfx_color_rgb565(or_, og_, ob_));
}

void gfx_blit_rgb565(const uint16_t *src, int x, int y, int w, int h, int src_stride)
{
    if (!src || w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int dy = y + j;
        if (dy < 0 || dy >= GFX_LCD_HEIGHT) continue;
        for (int i = 0; i < w; i++) {
            int dx = x + i;
            if (dx < 0 || dx >= GFX_LCD_WIDTH) continue;
            g_fb[dy * GFX_LCD_WIDTH + dx] = src[j * src_stride + i];
        }
    }
}

void gfx_blit_argb8888(const uint32_t *src, int x, int y, int w, int h, int src_stride)
{
    if (!src || w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int dy = y + j;
        if (dy < 0 || dy >= GFX_LCD_HEIGHT) continue;
        for (int i = 0; i < w; i++) {
            int dx = x + i;
            if (dx < 0 || dx >= GFX_LCD_WIDTH) continue;
            uint32_t p = src[j * src_stride + i];
            uint8_t a = (p >> 24) & 0xFF;
            uint8_t r = (p >> 16) & 0xFF;
            uint8_t g = (p >>  8) & 0xFF;
            uint8_t b = (p >>  0) & 0xFF;
            gfx_blend_pixel(dx, dy, r, g, b, a);
        }
    }
}
