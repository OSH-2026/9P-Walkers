#ifndef PWOS_SMALLPT_H
#define PWOS_SMALLPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 结果必须能装进 STM32 compute worker 的 320 字节静态缓冲区，也必须能
 * 通过 Mini9P 默认的 128 字节 iounit 一次写入 F429。
 */
#define PWOS_RENDER_PROTOCOL_VERSION 1u
#define PWOS_RENDER_REQUEST_LEN 18u
#define PWOS_RENDER_RESULT_HEADER_LEN 12u
#define PWOS_RENDER_TILE_MAX_WIDTH 8u
#define PWOS_RENDER_TILE_MAX_HEIGHT 7u
#define PWOS_RENDER_RESULT_MAX_LEN \
    (PWOS_RENDER_RESULT_HEADER_LEN + \
     PWOS_RENDER_TILE_MAX_WIDTH * PWOS_RENDER_TILE_MAX_HEIGHT * 2u)

#define PWOS_RENDER_SCENE_WHITTED 1u
#define PWOS_RENDER_FORMAT_RGB565_LE 1u

typedef struct {
    uint8_t scene_id;
    uint8_t tile_x;
    uint8_t tile_y;
    uint8_t tile_w;
    uint8_t tile_h;
    uint8_t image_w;
    uint8_t image_h;
    uint8_t samples;
    uint8_t max_depth;
    uint16_t frame_id;
    uint32_t seed;
    uint16_t camera_phase;
} pwos_render_request_t;

/* 解码并校验 Lua 调度器生成的固定长度请求。 */
int pwos_render_decode_request(
    const uint8_t *data,
    size_t len,
    pwos_render_request_t *out_request);

/* 返回完整 tile 结果长度：12 字节头部加 RGB565 像素。 */
uint16_t pwos_render_result_len(const pwos_render_request_t *request);

/* 写入可直接转发给 F429 /display/tile 的结果头部。 */
void pwos_render_write_result_header(
    const pwos_render_request_t *request,
    uint8_t *result);

/* 渲染 tile 内的一个像素，返回 RGB565。函数不分配内存且结果可复现。 */
uint16_t pwos_smallpt_render_pixel(
    const pwos_render_request_t *request,
    uint32_t tile_pixel_index);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_SMALLPT_H */
