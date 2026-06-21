/**
 * @file display_sync.h
 * @brief VSYNC-synchronized framebuffer swapping for the STM32F429 LTDC.
 *
 * Coordinates double-buffering between the CPU (drawing into the back
 * framebuffer in SDRAM) and the LTDC (scanning out the front framebuffer).
 * The swap is atomic and happens inside the LTDC vertical blanking, so the
 * panel never shows a half-drawn frame (eliminates tearing).
 */
#ifndef DISPLAY_SYNC_H_
#define DISPLAY_SYNC_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * Arm the LTDC line-event interrupt and record the address the LTDC is
 * currently scanning (so the first swap knows what to swap away from).
 * @param current_fb_addr  framebuffer address the LTDC layer 0 starts with
 */
void display_sync_init(uint32_t current_fb_addr);

/**
 * Request that the LTDC switch to @p addr at the next vertical blanking.
 * Non-blocking; pair with display_sync_wait() to block until the swap lands.
 */
void display_sync_request(uint32_t addr);

/**
 * Block the calling task until the LTDC line-event ISR fires and the
 * address swap completes. Returns after the LTDC has committed the new
 * framebuffer address (via HAL_LTDC_Reload with VERTICAL_BLANKING).
 */
void display_sync_wait(void);

#ifdef __cplusplus
}
#endif
#endif /* DISPLAY_SYNC_H_ */
