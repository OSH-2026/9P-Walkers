/**
 * @file display_sync.c
 * @brief LTDC line-event driven double-buffer swap.
 *
 * Flow:
 *   1. display_sync_init() programs a line event at the end of the active
 *      area (line = LTDC AccumulatedActiveH). The LTDC IRQ is already
 *      enabled by CubeMX and routes to HAL_LTDC_IRQHandler, which calls
 *      HAL_LTDC_LineEventCallback.
 *   2. display_sync_request(addr) stores the desired next framebuffer addr.
 *   3. At each vertical blanking the LTDC fires the line event. The callback
 *      checks if a swap is pending; if so it calls
 *      HAL_LTDC_SetAddress_NoReload + HAL_LTDC_Reload(VERTICAL_BLANKING)
 *      so the new address takes effect at the next blanking start, then
 *      gives the semaphore.
 *   4. display_sync_wait() blocks on the semaphore until the swap is done.
 *
 * The line event fires every frame (the IER LIE bit stays set), so even
 * if no swap is pending the task still wakes up at vsync rate — useful for
 * throttling the render loop to the display refresh.
 */
#include "display_sync.h"
#include "ltdc.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os.h"

/* Line where the active area ends (from ltdc.c: AccumulatedActiveH = 323).
 * Programming the line event here means the ISR runs right as the LTDC
 * finishes the last visible line and enters the vertical blanking porch,
 * which is the safe window for reloading the layer address. */
#define VSYNC_LINE              323u

static osSemaphoreId g_vsync_sem;
static volatile uint32_t g_current_addr;   /* what LTDC is showing now  */
static volatile uint32_t g_pending_addr;   /* what we want it to show   */

/* CMSIS-RTOS v1 semaphore attributes (static allocation). */
static osSemaphoreDef(g_vsync);

void display_sync_init(uint32_t current_fb_addr)
{
    g_vsync_sem = osSemaphoreCreate(osSemaphore(g_vsync), 1);
    /* Drain any initial count so the first wait blocks until a real vsync. */
    osSemaphoreWait(g_vsync_sem, 0);

    g_current_addr = current_fb_addr;
    g_pending_addr = current_fb_addr;   /* no swap requested yet */

    HAL_LTDC_ProgramLineEvent(&hltdc, VSYNC_LINE);
}

void display_sync_request(uint32_t addr)
{
    g_pending_addr = addr;
}

void display_sync_wait(void)
{
    osSemaphoreWait(g_vsync_sem, osWaitForever);
}

/* Called from HAL_LTDC_IRQHandler (LTDC_IRQn, priority 5). */
void HAL_LTDC_LineEventCallback(LTDC_HandleTypeDef *hltdc)
{
    if (g_pending_addr != g_current_addr) {
        HAL_LTDC_SetAddress_NoReload(hltdc, g_pending_addr, 0);
        HAL_LTDC_Reload(hltdc, LTDC_RELOAD_VERTICAL_BLANKING);
        g_current_addr = g_pending_addr;
    }

    /* Re-arm the line event for the next frame. */
    HAL_LTDC_ProgramLineEvent(hltdc, VSYNC_LINE);

    /* Wake the rendering task. */
    osSemaphoreRelease(g_vsync_sem);
}
