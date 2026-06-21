/**
 * @file gfx.h
 * @brief RGB565 graphics API for the STM32F429I-DISCO LCD.
 *
 * Resolution is 240x320 (portrait), pixel format RGB565 (2 bytes/pixel).
 * Two framebuffers live in external SDRAM (FB_A @ 0xD0000000, FB_B @
 * 0xD0025800). The LTDC scans out one ("front") while the CPU draws into the
 * other ("back"). gfx_present() + gfx_wait_vsync() swap them at the LTDC
 * vertical blanking, so partial frames never reach the panel (no tearing).
 *
 * Typical frame loop:
 *   gfx_init();
 *   for (;;) {
 *       gfx_clear(GFX_BLACK);
 *       ... draw into back buffer via gfx_put_pixel / gfx_fill_rect / etc ...
 *       gfx_present();        // hand back buffer to LTDC at next VSYNC
 *       gfx_wait_vsync();     // block until swap done; back buffer now flips
 *   }
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

/* Two framebuffers in SDRAM. FB_A matches the CubeMX LTDC layer 0 start
 * address; FB_B is the second buffer. Each is 150 KB. */
#define GFX_FB_A_ADDR     0xD0000000UL
#define GFX_FB_B_ADDR     (GFX_FB_A_ADDR + GFX_FB_BYTES)

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
 * Initialize both framebuffers (clear to black) and arm the LTDC line event
 * for VSYNC-synchronized swapping. Call after BSP_SDRAM_Init(),
 * BSP_LCD_Init(), and the LTDC is already running.
 */
void gfx_init(void);

/** Pointer to the back buffer (the surface you draw into). */
uint16_t *gfx_framebuffer(void);

/**
 * Hand the current back buffer to the LTDC. The actual address swap happens
 * at the next vertical blanking (inside the LTDC line-event ISR), so the
 * panel never shows a half-drawn frame.
 */
void gfx_present(void);

/**
 * Block until the LTDC finishes the current frame and the buffer swap
 * completes. After this returns, gfx_framebuffer() points to the OTHER
 * buffer (the one the LTDC just stopped showing), which is now safe to
 * overwrite for the next frame.
 */
void gfx_wait_vsync(void);

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
