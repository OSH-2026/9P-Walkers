/**
 * @file bsp_lcd.h
 * @brief ILI9341 display driver for the STM32F429I-DISCO.
 *
 * Hardware mapping on the F429I-DISCO board:
 *   - SPI5  : PF7=SCK, PF8=MISO, PF9=MOSI  (register configuration)
 *   - CS    : PC2  (CSX, active low)
 *   - D/CX  : PD13 (WRX_DCX, 0=command, 1=data)
 *   - RST   : PA7  (ACP_RST, active low)
 *   - LTDC  : RGB666 + HSYNC/VSYNC/DOTCLK/DE (pixel streaming)
 *   - SDRAM : 0xD0000000 framebuffer (240x320 RGB565)
 *
 * The ILI9341 is initialized via SPI into RGB-interface mode, then the LTDC
 * streams pixels continuously. CPU drawing targets the SDRAM framebuffer
 * through the gfx module; this driver only handles panel power-up and mode
 * selection.
 */
#ifndef BSP_LCD_H_
#define BSP_LCD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Panel geometry (matches CubeMX LTDC layer 1). */
#define BSP_LCD_WIDTH    240
#define BSP_LCD_HEIGHT   320

/**
 * Power up the ILI9341 via SPI5 and switch it to RGB interface mode so the
 * LTDC stream is displayed. Must run AFTER MX_SPI5_Init() and MX_LTDC_Init().
 * @return 0 on success.
 */
int BSP_LCD_Init(void);

#ifdef __cplusplus
}
#endif
#endif /* BSP_LCD_H_ */
