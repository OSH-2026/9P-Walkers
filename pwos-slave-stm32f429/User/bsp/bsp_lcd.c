/**
 * @file bsp_lcd.c
 * @brief ILI9341 panel bring-up for the STM32F429I-DISCO.
 *
 * Uses the exact init sequence from ST's official BSP
 * (STMicroelectronics/32f429idiscovery-bsp + STMicroelectronics/stm32-ili9341).
 *
 * Hardware: SPI5 (PF7=SCK, PF9=MOSI) for register config, LTDC for RGB streaming.
 *   LCD_CS  = PC2 (GPIO output, active low)
 *   LCD_DCX = PD13 (GPIO output, 0=command, 1=data)
 *   LCD_RST = none (power-on reset only; PA7 is audio codec, NOT LCD)
 *
 * The two registers that make RGB mode work (were missing in v1):
 *   0xB0 RGB_INTERFACE  → 0xC2  (enable RGB interface, DE mode)
 *   0xF6 INTERFACE      → 0x01, 0x00, 0x06  (16-bit RGB interface)
 */
#include "bsp_lcd.h"
#include "stm32f4xx_hal.h"
#include "spi.h"
#include "main.h"
#include <stdint.h>

/* LCD control pins (from ST BSP stm32f429i_discovery.h). */
#define LCD_CS_GPIO_Port    CSX_GPIO_Port       /* GPIOC */
#define LCD_CS_PIN          CSX_Pin             /* PC2   */
#define LCD_DCX_GPIO_Port   WRX_DCX_GPIO_Port   /* GPIOD */
#define LCD_DCX_PIN         WRX_DCX_Pin         /* PD13  */

/* Gyro CS on same SPI5 bus — must deselect before talking to LCD. */
#define GYRO_CS_GPIO_Port   NCS_MEMS_SPI_GPIO_Port  /* GPIOC */
#define GYRO_CS_PIN         NCS_MEMS_SPI_Pin        /* PC1   */

static inline void lcd_cs_low(void)  { HAL_GPIO_WritePin(LCD_CS_GPIO_Port,  LCD_CS_PIN,  GPIO_PIN_RESET); }
static inline void lcd_cs_high(void) { HAL_GPIO_WritePin(LCD_CS_GPIO_Port,  LCD_CS_PIN,  GPIO_PIN_SET);   }
static inline void lcd_dcx_cmd(void) { HAL_GPIO_WritePin(LCD_DCX_GPIO_Port, LCD_DCX_PIN, GPIO_PIN_RESET); }
static inline void lcd_dcx_dat(void) { HAL_GPIO_WritePin(LCD_DCX_GPIO_Port, LCD_DCX_PIN, GPIO_PIN_SET);   }

static void lcd_write_cmd(uint8_t cmd)
{
    lcd_dcx_cmd();
    lcd_cs_low();
    HAL_SPI_Transmit(&hspi5, &cmd, 1, HAL_MAX_DELAY);
    lcd_cs_high();
}

static void lcd_write_data(uint8_t dat)
{
    lcd_dcx_dat();
    lcd_cs_low();
    HAL_SPI_Transmit(&hspi5, &dat, 1, HAL_MAX_DELAY);
    lcd_cs_high();
}

static void lcd_write_seq(uint8_t cmd, const uint8_t *data, uint8_t n)
{
    lcd_write_cmd(cmd);
    for (uint8_t i = 0; i < n; i++) lcd_write_data(data[i]);
}

/* ---- ILI9341 init tables (exact values from ST BSP ili9341.c::ili9341_Init) ---- */

static const uint8_t SEQ_CA[]  = { 0xC3, 0x08, 0x50 };              /* 0xCA custom          */
static const uint8_t SEQ_CF[]  = { 0x00, 0xC1, 0x30 };              /* 0xCF Power control B */
static const uint8_t SEQ_ED[]  = { 0x64, 0x03, 0x12, 0x81 };        /* 0xED Power-on seq    */
static const uint8_t SEQ_E8[]  = { 0x85, 0x00, 0x78 };              /* 0xE8 Driver timing A */
static const uint8_t SEQ_CB[]  = { 0x39, 0x2C, 0x00, 0x34, 0x02 };  /* 0xCB Power control A */
static const uint8_t SEQ_F7[]  = { 0x20 };                          /* 0xF7 Pump ratio ctrl */
static const uint8_t SEQ_EA[]  = { 0x00, 0x00 };                    /* 0xEA Driver timing B */
static const uint8_t SEQ_B1[]  = { 0x00, 0x1B };                    /* 0xB1 Frame rate      */
static const uint8_t SEQ_B6_1[]= { 0x0A, 0xA2 };                    /* 0xB6 DFC (first)     */
static const uint8_t SEQ_C0[]  = { 0x10 };                          /* 0xC0 Power control 1 */
static const uint8_t SEQ_C1[]  = { 0x10 };                          /* 0xC1 Power control 2 */
static const uint8_t SEQ_C5[]  = { 0x45, 0x15 };                    /* 0xC5 VCOM control 1  */
static const uint8_t SEQ_C7[]  = { 0x90 };                          /* 0xC7 VCOM control 2  */
static const uint8_t SEQ_36[]  = { 0xC8 };                          /* 0x36 MADCTL          */
static const uint8_t SEQ_F2[]  = { 0x00 };                          /* 0xF2 3-gamma disable */
static const uint8_t SEQ_B0[]  = { 0xC2 };                          /* 0xB0 RGB interface   */
static const uint8_t SEQ_B6_2[]= { 0x0A, 0xA7, 0x27, 0x04 };        /* 0xB6 DFC (second)    */
static const uint8_t SEQ_2A[]  = { 0x00, 0x00, 0x00, 0xEF };        /* 0x2A Column addr     */
static const uint8_t SEQ_2B[]  = { 0x00, 0x00, 0x01, 0x3F };        /* 0x2B Page addr       */
static const uint8_t SEQ_F6[]  = { 0x01, 0x00, 0x06 };              /* 0xF6 Interface ctrl  */
static const uint8_t SEQ_26[]  = { 0x01 };                          /* 0x26 Gamma curve     */

static const uint8_t SEQ_E0[]  = {                                  /* 0xE0 Positive gamma  */
    0x0F, 0x29, 0x24, 0x0C, 0x0E, 0x09, 0x4E, 0x78,
    0x3C, 0x09, 0x13, 0x05, 0x17, 0x11, 0x00
};
static const uint8_t SEQ_E1[]  = {                                  /* 0xE1 Negative gamma  */
    0x00, 0x16, 0x1B, 0x04, 0x11, 0x07, 0x31, 0x33,
    0x42, 0x05, 0x0C, 0x0A, 0x28, 0x2F, 0x0F
};

int BSP_LCD_Init(void)
{
    /* Deselect gyro (shares SPI5 bus) and deselect LCD before starting. */
    HAL_GPIO_WritePin(GYRO_CS_GPIO_Port, GYRO_CS_PIN, GPIO_PIN_SET);
    lcd_cs_high();

    /* Power-on register sequence (matches ST BSP ili9341_Init exactly). */
    lcd_write_seq(0xCA, SEQ_CA,   sizeof(SEQ_CA));
    lcd_write_seq(0xCF, SEQ_CF,   sizeof(SEQ_CF));
    lcd_write_seq(0xED, SEQ_ED,   sizeof(SEQ_ED));
    lcd_write_seq(0xE8, SEQ_E8,   sizeof(SEQ_E8));
    lcd_write_seq(0xCB, SEQ_CB,   sizeof(SEQ_CB));
    lcd_write_seq(0xF7, SEQ_F7,   sizeof(SEQ_F7));
    lcd_write_seq(0xEA, SEQ_EA,   sizeof(SEQ_EA));
    lcd_write_seq(0xB1, SEQ_B1,   sizeof(SEQ_B1));
    lcd_write_seq(0xB6, SEQ_B6_1, sizeof(SEQ_B6_1));
    lcd_write_seq(0xC0, SEQ_C0,   sizeof(SEQ_C0));
    lcd_write_seq(0xC1, SEQ_C1,   sizeof(SEQ_C1));
    lcd_write_seq(0xC5, SEQ_C5,   sizeof(SEQ_C5));
    lcd_write_seq(0xC7, SEQ_C7,   sizeof(SEQ_C7));
    lcd_write_seq(0x36, SEQ_36,   sizeof(SEQ_36));
    lcd_write_seq(0xF2, SEQ_F2,   sizeof(SEQ_F2));

    /* RGB interface signal control — this switches the ILI9341 into RGB
     * display mode so the LTDC pixel stream is shown. */
    lcd_write_seq(0xB0, SEQ_B0,   sizeof(SEQ_B0));

    /* Display function control (second write with extended values). */
    lcd_write_seq(0xB6, SEQ_B6_2, sizeof(SEQ_B6_2));

    /* Column / page address set (full 240x320 panel). */
    lcd_write_seq(0x2A, SEQ_2A,   sizeof(SEQ_2A));
    lcd_write_seq(0x2B, SEQ_2B,   sizeof(SEQ_2B));

    /* Interface control: 16-bit RGB interface mode. */
    lcd_write_seq(0xF6, SEQ_F6,   sizeof(SEQ_F6));

    /* GRAM start + mandatory 200 ms wait (per ST BSP). */
    lcd_write_cmd(0x2C);
    HAL_Delay(200);

    /* Gamma. */
    lcd_write_seq(0x26, SEQ_26,   sizeof(SEQ_26));
    lcd_write_seq(0xE0, SEQ_E0,   sizeof(SEQ_E0));
    lcd_write_seq(0xE1, SEQ_E1,   sizeof(SEQ_E1));

    /* Sleep out → wait 200 ms → display on. */
    lcd_write_cmd(0x11);
    HAL_Delay(200);
    lcd_write_cmd(0x29);
    lcd_write_cmd(0x2C);

    return 0;
}
