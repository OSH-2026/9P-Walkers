# 显示屏驱动与图形 API

STM32F429I-DISCO 板载 ILI9341 240×320 TFT LCD 的驱动适配、双缓冲渲染管线，以及如何用它绘制任意图像。

## 硬件映射

| 信号 | 引脚 | 说明 |
|------|------|------|
| SPI5 SCK/MOSI | PF7 / PF9 | ILI9341 寄存器配置（初始化阶段） |
| LCD_CS | PC2 | SPI 片选，低有效 |
| LCD_DCX | PD13 | 数据/命令选择，0=命令, 1=数据 |
| LTDC RGB | 见 `Core/Src/ltdc.c` | RGB666 + HSYNC/VSYNC/DOTCLK/DE 并行像素流 |
| SDRAM | 0xD0000000 | IS42S16400J, 8MB, 帧缓冲驻留于此 |

ILI9341 通过 SPI5 接收初始化序列后切换到 RGB 接口模式，之后 LTDC 持续从 SDRAM 扫描像素流到屏幕，CPU 不再需要逐帧发送命令。

## 目录结构

```
User/
├── bsp/
│   ├── bsp_sdram.h/c        SDRAM 芯片初始化（FMC 已由 CubeMX 配置）
│   ├── bsp_lcd.h/c          ILI9341 寄存器初始化（SPI5 → RGB 模式）
│   └── display_sync.h/c     LTDC 行中断驱动的 VSYNC 双缓冲交换
├── gfx/
│   └── gfx.h/c              RGB565 图形 API（画点/线/矩形/三角形/blit）
└── app/
    └── cube_demo.h/c        旋转立方体演示（Cortex-M4F 软件光栅化）
```

## 渲染管线

```
                SDRAM (0xD0000000)
                ┌──────────────────┐
   CPU 写 ───→  │  FB_A (150 KB)   │
                ├──────────────────┤
                │  FB_B (150 KB)   │
                └────────┬─────────┘
                         │ HAL_LTDC_SetAddress_NoReload
                         │ + HAL_LTDC_Reload(VERTICAL_BLANKING)
                         ↓
                ┌──────────────────┐
   LTDC 扫描 ←──│  前台帧缓冲      │──→ ILI9341 RGB 接口 ──→ 屏幕
                └──────────────────┘
```

**双缓冲 + VSYNC 同步**消除撕裂：CPU 始终画到后台缓冲，LTDC 始终扫描前台缓冲。`gfx_present()` 把后台地址交给 `display_sync`，在 LTDC 行中断里（垂直消隐期）原子交换层地址。屏幕永远不会看到半帧画面。

## 帧循环模板

```c
#include "bsp_sdram.h"
#include "bsp_lcd.h"
#include "gfx.h"

void my_render_task(void) {
    BSP_SDRAM_Init();
    BSP_LCD_Init();
    gfx_init();

    for (;;) {
        gfx_clear(GFX_BLACK);
        /* 在这里画你想要的东西，所有绘制都进入后台缓冲 */
        gfx_fill_rect(10, 10, 50, 50, GFX_RED);
        gfx_draw_line(0, 0, 239, 319, GFX_WHITE);
        /* ... */

        gfx_present();      /* 请求在下一个 VSYNC 交换到前台 */
        gfx_wait_vsync();   /* 阻塞直到交换完成；此后后台缓冲翻转 */
    }
}
```

`gfx_wait_vsync()` 返回后，`gfx_framebuffer()` 自动指向另一个缓冲（LTDC 刚释放的那个），可以直接覆盖。帧率上限等于 LTDC 刷新率（约 60 Hz）。

## 如何绘制任意图像

### 方法 1：直接 RGB565 blit（最快）

把图像以 RGB565 格式存成 C 数组，调用 `gfx_blit_rgb565` 直接拷贝到帧缓冲。这是渲染任意位图的主路径。

```c
/* 一张 16×16 的红色方块，RGB565 格式 */
static const uint16_t my_image[16 * 16] = { /* ... 像素数据 ... */ };

gfx_blit_rgb565(my_image,        /* 源数据              */
                100, 50,         /* 目标左上角 (x, y)    */
                16, 16,          /* 图像宽高             */
                16);             /* 源行步长（每行像素数）*/
```

**如何把 PNG/JPG 转成 RGB565 C 数组：**

1. 用 ImageMagick / GIMP / 在线工具把图片缩放到目标尺寸（≤ 240×320）。
2. 转成 RGB565 原始二进制：
   ```bash
   convert input.png -resize 240x320! -depth 16 RGB565:output.bin
   ```
3. 用 `xxd -i` 转成 C 数组：
   ```bash
   xxd -i output.bin > image_data.h
   ```
4. 在代码里 `#include "image_data.h"` 然后把数组指针传给 `gfx_blit_rgb565`（注意 `xxd` 生成的是 `uint8_t[]`，传给 API 时按 `(const uint16_t*)` 强转即可，因为 RGB565 是 2 字节/像素）。

源行步长 `src_stride` 允许源图带 padding（比如把多张图拼在一张大 atlas 里，从中间取一块）：

```c
/* 从一张 256×256 atlas 里取 (32,16) 位置的 64×64 子图 */
gfx_blit_rgb565(&atlas[16 * 256 + 32],  /* 起始像素指针 */
                0, 0, 64, 64,           /* 目标位置 + 取图大小 */
                256);                    /* atlas 行步长 */
```

### 方法 2：带 Alpha 通道的 ARGB8888 blit

支持逐像素半透明混合，适合精灵叠加、UI 覆盖层：

```c
static const uint32_t sprite[32 * 32] = { /* 0xAARRGGBB 格式 */ };

gfx_blit_argb8888(sprite, 80, 80, 32, 32, 32);
```

转换工具：
```bash
convert input.png -resize 32x32! -depth 32 RGBA:output.bin
xxd -i output.bin > sprite.h
```

Alpha=0 的像素完全透明（不改变背景），Alpha=255 完全不透明，中间值做线性混合。

### 方法 3：矢量图元

不需要位图时，用图元 API 现场画：

| API | 用途 |
|-----|------|
| `gfx_put_pixel(x,y,c)` | 单像素 |
| `gfx_fill_rect(x,y,w,h,c)` | 实心矩形 |
| `gfx_draw_rect(x,y,w,h,c)` | 矩形边框 |
| `gfx_draw_line(x0,y0,x1,y1,c)` | Bresenham 直线 |
| `gfx_draw_line_thick(...,t,c)` | 粗直线（用于线框） |
| `gfx_fill_triangle(...)` | 实心三角形（3D 光栅化基础） |

### 方法 4：直接操作帧缓冲

复杂场景（比如手写 shader、效果叠加）可以直接拿指针写：

```c
uint16_t *fb = gfx_framebuffer();
for (int y = 0; y < GFX_LCD_HEIGHT; y++) {
    for (int x = 0; x < GFX_LCD_WIDTH; x++) {
        fb[y * GFX_LCD_WIDTH + x] = GFX_RGB565(x, y, (x+y) & 0xFF);
    }
}
```

帧缓冲布局：行优先，`fb[y * GFX_LCD_WIDTH + x]`，RGB565（R5 G6 B5）。

## 颜色

```c
gfx_color_t c = GFX_RGB565(255, 128, 0);   /* 自定义 RGB → RGB565 */
/* 预定义：GFX_BLACK GFX_WHITE GFX_RED GFX_GREEN GFX_BLUE
   GFX_YELLOW GFX_CYAN GFX_MAGENTA GFX_ORANGE GFX_DARKBLUE ... */
```

## 完整 API 列表

```c
/* 初始化与帧循环 */
void  gfx_init(void);
uint16_t *gfx_framebuffer(void);
void  gfx_present(void);
void  gfx_wait_vsync(void);

/* 图元 */
void  gfx_clear(gfx_color_t color);
void  gfx_put_pixel(int x, int y, gfx_color_t color);
void  gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);
void  gfx_draw_rect(int x, int y, int w, int h, gfx_color_t color);
void  gfx_draw_line(int x0, int y0, int x1, int y1, gfx_color_t color);
void  gfx_draw_line_thick(int x0, int y0, int x1, int y1, int thickness, gfx_color_t color);
void  gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, gfx_color_t color);

/* 图像 blit */
void  gfx_blit_rgb565(const uint16_t *src, int x, int y, int w, int h, int src_stride);
void  gfx_blit_argb8888(const uint32_t *src, int x, int y, int w, int h, int src_stride);
void  gfx_blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* 颜色 */
gfx_color_t gfx_color_rgb565(uint8_t r, uint8_t g, uint8_t b);
```

所有图元调用都做边界裁剪，越界坐标不会崩溃。

## 性能

- 帧缓冲在 SDRAM（FMC 16-bit 总线），CPU 写入受总线带宽限制
- 全屏 `gfx_clear` 约 3–5 ms（150 KB）
- 旋转立方体单帧（含三角填充 + 边线）约 8–12 ms
- 帧率受 LTDC 刷新率限制（~60 Hz），渲染快于刷新时 `gfx_wait_vsync` 自动节流
- 若需要更快填充，可启用 DMA2D（`Core/Src/dma2d.c` 已初始化）做内存到内存拷贝，目前未在 gfx 中使用

## 编译与烧录

```bash
cmake --preset Debug
cmake --build --preset Debug
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program build/Debug/pwos-slave-stm32f429.elf verify reset exit"
```

## 参考资料

- ILI9341 初始化序列：`STMicroelectronics/32f429idiscovery-bsp` + `STMicroelectronics/stm32-ili9341`
- LTDC 双缓冲：STM32F4 参考手册 RM0090 §14 TFT LCD controller
- SDRAM 初始化：IS42S16400J 数据手册 + ST BSP `stm32f429i_discovery_sdram.c`
