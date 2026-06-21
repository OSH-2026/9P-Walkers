/**
 * @file gfx.h
 * @brief RGB565 graphics API for the STM32F429I-DISCO LCD.
 *
 * Operates directly on the LTDC framebuffer in external SDRAM (0xD0000000).
 * Resolution is 240x320 (portrait), pixel format RGB565 (2 bytes/pixel).
 *
 * The API is the "interface for drawing arbitrary images" the user asked for:
 *   - gfx_init()                 : bind framebuffer, clear screen
 *   - gfx_clear(color)           : fill whole screen
 *   - gfx_put_pixel(x,y,c)       : single pixel
 *   - gfx_fill_rect(x,y,w,h,c)   : filled rectangle (the main blit primitive)
 *   - gfx_draw_rect(x,y,w,h,c)   : rectangle outline
 *   - gfx_draw_line(x0,y0,x1,y1,c) : Bresenham line
 *   - gfx_draw_line_wide(...,thick,c) : thick line (used by cube wireframe)
 *   - gfx_fill_triangle(...)     : flat-split triangle rasterizer
 *   - gfx_blit_rgb565(...)       : blit an external RGB565 buffer (arbitrary image)
 *   - gfx_blit_argb8888(...)     : blit an external ARGB8888 buffer w/ alpha blend
 *   - gfx_color_rgb565(r,g,b)    : 24-bit -> RGB565
 *   - gfx_swap_buffers()         : double-buffer commit (layer 1 <-> layer 2)
 *
 * Coordinate system: (0,0) at top-left, x grows right, y grows down.
 * Bounds-checked; out-of-range draws are silently clipped.
 */
#ifndef GFX_H_
#define GFX_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Display geometry (must match CubeMX LTDC layer config). */
#define GFX_LCD_WIDTH     240
#define GFX_LCD_HEIGHT    320
#define GFX_FB_PIXELS     (GFX_LCD_WIDTH * GFX_LCD_HEIGHT)
#define GFX_FB_BYTES      (GFX_FB_PIXELS * 2)

typedef uint16_t gfx_color_t;  /* RGB565 */

/* Color helpers (8-bit channels -> RGB565).
 * The macro form is a constant expression so it can initialize static arrays. */
#define GFX_RGB565(r, g, b) \
    (gfx_color_t)((((uint16_t)(r) & 0xF8) << 8) | (((uint16_t)(g) & 0xFC) << 3) | ((uint16_t)(b) >> 3))

static inline gfx_color_t gfx_color_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return GFX_RGB565(r, g, b);
}

/* Predefined palette (all are constant expressions). */
#define GFX_BLACK       ((gfx_color_t)GFX_RGB565(  0,   0,   0))
#define GFX_WHITE       ((gfx_color_t)GFX_RGB565(255, 255, 255))
#define GFX_RED         ((gfx_color_t)GFX_RGB565(255,   0,   0))
#define GFX_GREEN       ((gfx_color_t)GFX_RGB565(  0, 255,   0))
#define GFX_BLUE        ((gfx_color_t)GFX_RGB565(  0,   0, 255))
#define GFX_YELLOW      ((gfx_color_t)GFX_RGB565(255, 255,   0))
#define GFX_CYAN        ((gfx_color_t)GFX_RGB565(  0, 255, 255))
#define GFX_MAGENTA     ((gfx_color_t)GFX_RGB565(255,   0, 255))
#define GFX_ORANGE      ((gfx_color_t)GFX_RGB565(255, 140,   0))
#define GFX_GRAY50      ((gfx_color_t)GFX_RGB565(128, 128, 128))
#define GFX_DARKBLUE    ((gfx_color_t)GFX_RGB565(  0,   0,  64))

/**
 * Bind the active framebuffer and clear it to black.
 * Call after BSP_SDRAM_Init() and before any draw call.
 */
void gfx_init(void);

/** Pointer to the currently-active drawing surface (RGB565, GFX_LCD_WIDTH*height). */
uint16_t *gfx_framebuffer(void);

/** Clear the whole screen to a single color. */
void gfx_clear(gfx_color_t color);

/** Set a single pixel. Bounds-checked. */
void gfx_put_pixel(int x, int y, gfx_color_t color);

/** Filled rectangle. Bounds-checked. */
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);

/** Rectangle outline. Bounds-checked. */
void gfx_draw_rect(int x, int y, int w, int h, gfx_color_t color);

/** Bresenham line. Bounds-checked (clipped per-pixel). */
void gfx_draw_line(int x0, int y0, int x1, int y1, gfx_color_t color);

/** Thick line drawn as an axis-aligned "box" trace (good enough for cube edges). */
void gfx_draw_line_thick(int x0, int y0, int x1, int y1, int thickness, gfx_color_t color);

/** Filled triangle (split into flat-top + flat-bottom). Bounds-checked. */
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, gfx_color_t color);

/** Alpha-blend a single ARGB8888 pixel over the framebuffer (used by gfx_blit_argb8888). */
void gfx_blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/**
 * Blit an external RGB565 image directly. No scaling; source must fit on screen.
 * @param src        pointer to RGB565 source buffer
 * @param x,y        top-left destination
 * @param w,h        source dimensions in pixels
 * @param src_stride pixels per source row (>= w; allows padding)
 */
void gfx_blit_rgb565(const uint16_t *src, int x, int y, int w, int h, int src_stride);

/**
 * Blit an external ARGB8888 image with per-pixel alpha blending.
 * @param src        pointer to ARGB8888 source buffer (0xAARRGGBB packed as uint32_t)
 * @param x,y        top-left destination
 * @param w,h        source dimensions in pixels
 * @param src_stride pixels per source row
 */
void gfx_blit_argb8888(const uint32_t *src, int x, int y, int w, int h, int src_stride);

#ifdef __cplusplus
}
#endif
#endif /* GFX_H_ */
