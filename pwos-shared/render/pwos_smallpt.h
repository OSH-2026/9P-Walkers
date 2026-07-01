#ifndef PWOS_SMALLPT_H
#define PWOS_SMALLPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 协议 v3: tile_x/tile_y 也升级为 le16，支持 320+ 行分辨率。
 * 请求 22 字节，结果头 16 字节，tile 16x7 最大结果 240 字节。
 */
#define PWOS_RENDER_PROTOCOL_VERSION 3u
#define PWOS_RENDER_REQUEST_LEN 22u
#define PWOS_RENDER_RESULT_HEADER_LEN 16u
#define PWOS_RENDER_TILE_MAX_WIDTH 16u
#define PWOS_RENDER_TILE_MAX_HEIGHT 7u
#define PWOS_RENDER_RESULT_MAX_LEN \
    (PWOS_RENDER_RESULT_HEADER_LEN + \
     PWOS_RENDER_TILE_MAX_WIDTH * PWOS_RENDER_TILE_MAX_HEIGHT * 2u)

#define PWOS_RENDER_SCENE_WHITTED 1u
#define PWOS_RENDER_FORMAT_RGB565_LE 1u

typedef struct {
    uint8_t scene_id;
    uint16_t tile_x;
    uint16_t tile_y;
    uint8_t tile_w;
    uint8_t tile_h;
    uint16_t image_w;
    uint16_t image_h;
    uint8_t samples;
    uint8_t max_depth;
    uint16_t frame_id;
    uint32_t seed;
    uint16_t camera_phase;
} pwos_render_request_t;

int pwos_render_decode_request(
    const uint8_t *data,
    size_t len,
    pwos_render_request_t *out_request);

uint16_t pwos_render_result_len(const pwos_render_request_t *request);

void pwos_render_write_result_header(
    const pwos_render_request_t *request,
    uint8_t *result);

uint16_t pwos_smallpt_render_pixel(
    const pwos_render_request_t *request,
    uint32_t tile_pixel_index);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_SMALLPT_H */

