#ifndef PWOS_RENDER_DISPLAY_H
#define PWOS_RENDER_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWOS_RENDER_CANVAS_WIDTH  240u
#define PWOS_RENDER_CANVAS_HEIGHT 320u

typedef struct {
    uint8_t initialized;
    uint8_t dirty;
    uint16_t frame_id;
    uint32_t tiles_received;
    uint32_t frames_presented;
    uint32_t rejected_tiles;
} pwos_render_display_status_t;

int pwos_render_display_init(void);
int pwos_render_display_apply_tile(const uint8_t *data, uint16_t len);
int pwos_render_display_step(void);
void pwos_render_display_get_status(pwos_render_display_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_RENDER_DISPLAY_H */
