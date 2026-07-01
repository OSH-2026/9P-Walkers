# STM32F429 显示运行时

STM32F429I-DISCO 使用板载 ILI9341 240x320 LCD、LTDC 和外部 SDRAM。当前显示入口是
mini9P `/display/tile`，用于接收分布式渲染结果。

## 硬件

| 部件 | 配置 |
|---|---|
| ILI9341 配置接口 | SPI5，PF7/PF9，CS=PC2，DCX=PD13 |
| 像素接口 | LTDC RGB 并行 |
| SDRAM | `0xD0000000`，8 MiB |
| 帧格式 | RGB565 |
| 分辨率 | 240x320 |

## 软件层

```text
User/bsp/bsp_sdram       SDRAM 初始化
User/bsp/bsp_lcd         ILI9341 初始化并切到 RGB 模式
User/bsp/display_sync    VSYNC 双缓冲交换
User/gfx/gfx             RGB565 framebuffer API
User/app/render_display  tile 校验、复制、present 和状态
```

立方体 demo 和未接入的 littlefs/SD 显示实验代码已删除。

## Tile 路径

P4 Lua 调度器提交 `raytrace_tile` Job。任意 STM32 可以计算 tile，结果由 P4 写入
F429：

```text
Twrite /display/tile
  -> local_vfs
  -> render_display_write_tile
  -> 后台 framebuffer
  -> gfx_present
  -> VSYNC 时切换 LTDC 地址
```

tile header 包含协议版本、区域坐标、宽高和 frame ID；payload 是连续 RGB565 像素。
写入前必须校验边界、长度和协议版本，越界 tile 不得修改 framebuffer。

## 诊断

```text
cat /mcu3/display/status
cat /mcu3/compute/jobs
cat /mcu3/sys/tasks
```

`/display/status` 提供最近 frame/tile、接收数量、丢弃数量和 present 次数。

## 图形 API

```c
void gfx_init(void);
uint16_t *gfx_framebuffer(void);
void gfx_present(void);
void gfx_wait_vsync(void);
void gfx_clear(gfx_color_t color);
void gfx_put_pixel(int x, int y, gfx_color_t color);
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);
void gfx_draw_line(int x0, int y0, int x1, int y1, gfx_color_t color);
void gfx_blit_rgb565(const uint16_t *src, int x, int y,
                     int w, int h, int src_stride);
```

所有图元做边界裁剪。复杂渲染可直接写 `gfx_framebuffer()` 返回的行优先 RGB565 缓冲。

## 构建

```bash
cd pwos-slave-stm32f429
cmake --preset Debug
cmake --build --preset Debug
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program build/Debug/pwos-slave-stm32f429.elf verify reset exit"
```
