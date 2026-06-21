/**
 * @file cube_demo.h
 * @brief Rotating 3D cube demo for the STM32F429I-DISCO LCD.
 *
 * A self-contained software 3D renderer: rotates a cube around X + Y axes,
 * perspective-projects vertices to 2D, back-face culls, flat-shades visible
 * faces by the dot product of the face normal with a fixed light direction,
 * then outlines edges with thick lines.
 *
 * Uses Cortex-M4F hardware float (sinf/cosf from libm); no lookup tables.
 */
#ifndef CUBE_DEMO_H_
#define CUBE_DEMO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * Render one frame at the given angles and present it.
 * @param angle_x  rotation around X axis (radians)
 * @param angle_y  rotation around Y axis (radians)
 */
void cube_demo_render(float angle_x, float angle_y);

/** Convenience: render + advance internal angle state. Call once per tick. */
void cube_demo_step(void);

#ifdef __cplusplus
}
#endif
#endif /* CUBE_DEMO_H_ */
