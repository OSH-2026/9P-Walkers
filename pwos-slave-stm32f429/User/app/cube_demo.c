/**
 * @file cube_demo.c
 * @brief Rotating cube: software 3D renderer using the Cortex-M4F FPU.
 *
 * Pipeline per frame:
 *   1. Define 8 cube vertices and 12 triangles (2 per face).
 *   2. Rotate each vertex around X then Y (matrices below).
 *   3. Translate the cube to be in front of the camera (z += cam_dist).
 *   4. Perspective project:  sx = x * f/(z);  sy = y * f/(z)
 *   5. For each triangle compute signed screen area -> back-face cull
 *      (counter-clockwise = visible).
 *   6. Compute face normal in 3D, dot with light dir -> lambert factor.
 *      Shade the face color by the factor (0.2 .. 1.0 ambient clamp).
 *   7. Fill the triangle, then outline the cube's visible edges in white.
 *
 * The rotation/projection math follows the standard approach used by every
 * "spinning cube on MCU" reference (e.g. the stm32_tiny_engine demos,
 * Shawn Hymel's "SwissArmyDice" SPIFFS 3D cube, and the classic
 * "3D Graphics on MCU" tutorials).  Nothing here is proprietary.
 */
#include "cube_demo.h"
#include "gfx.h"
#include <math.h>

/* ----- Projection geometry ----------------------------------------------- */
/* Screen is 240x320 portrait; cube is centered at (120, 160).
 * Focal length f and camera distance were tuned so the cube fills ~40% of
 * the panel and never clips when rotated. */
#define DEMO_CX            (GFX_LCD_WIDTH  / 2)   /* 120 */
#define DEMO_CY            (GFX_LCD_HEIGHT / 2)   /* 160 */
#define DEMO_FOCAL         180.0f
#define DEMO_CAM_DIST      3.5f
#define DEMO_CUBE_SIZE     1.0f                   /* half-edge in world units */

/* ----- Cube geometry ----------------------------------------------------- */
/* 8 vertices of an axis-aligned cube, [-s, +s]^3. */
static const float CUBE_V[8][3] = {
    { -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 },
    { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 },
};

/* 12 triangles: (v0, v1, v2), wound counter-clockwise when viewed from
 * outside the cube. Index into CUBE_V[]. */
static const int CUBE_TRI[12][3] = {
    /* +X face (right)  */ { 1, 5, 6 }, { 1, 6, 2 },
    /* -X face (left)   */ { 4, 0, 3 }, { 4, 3, 7 },
    /* +Y face (top)    */ { 3, 2, 6 }, { 3, 6, 7 },
    /* -Y face (bottom) */ { 0, 4, 5 }, { 0, 5, 1 },
    /* +Z face (front)  */ { 5, 4, 7 }, { 5, 7, 6 },
    /* -Z face (back)   */ { 0, 1, 2 }, { 0, 2, 3 },
};

/* 12 cube edges, drawn as thick white lines on top of the shaded faces. */
static const int CUBE_EDGE[12][2] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, /* back face */
    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, /* front face */
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }, /* side edges */
};

/* Per-face base color (RGB565). Order matches the 6 faces above (2 tris each). */
static const gfx_color_t FACE_COLORS[6] = {
    GFX_RED,     /* +X */
    GFX_BLUE,    /* -X */
    GFX_GREEN,   /* +Y */
    GFX_YELLOW,  /* -Y */
    GFX_CYAN,    /* +Z */
    GFX_MAGENTA, /* -Z */
};

/* Light direction (world space, normalized). Points "up and toward camera". */
static const float LIGHT_DIR[3] = { 0.0f, 0.7071f, -0.7071f };

/* ----- Small vector helpers ---------------------------------------------- */
static inline float vdot(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static inline void vcross(float r[3], const float a[3], const float b[3])
{
    r[0] = a[1]*b[2] - a[2]*b[1];
    r[1] = a[2]*b[0] - a[0]*b[2];
    r[2] = a[0]*b[1] - a[1]*b[0];
}

static inline void vsub(float r[3], const float a[3], const float b[3])
{
    r[0] = a[0]-b[0]; r[1] = a[1]-b[1]; r[2] = a[2]-b[2];
}

static inline float vlen(const float a[3])
{
    return sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
}

/* ----- Shader ------------------------------------------------------------ */
/* Multiply a base color by a scalar factor (0..1) with ambient floor. */
static gfx_color_t shade(gfx_color_t base, float factor)
{
    if (factor < 0.2f) factor = 0.2f;   /* ambient floor */
    if (factor > 1.0f) factor = 1.0f;
    /* Decompose RGB565 -> scale each channel -> recompose. */
    uint8_t r = (base >> 11) & 0x1F;
    uint8_t g = (base >>  5) & 0x3F;
    uint8_t b = (base      ) & 0x1F;
    r = (uint8_t)(r * factor);
    g = (uint8_t)(g * factor);
    b = (uint8_t)(b * factor);
    return (gfx_color_t)((r << 11) | (g << 5) | b);
}

/* ----- Renderer ---------------------------------------------------------- */
void cube_demo_render(float angle_x, float angle_y)
{
    /* Precompute sin/cos once per frame. */
    float sx = sinf(angle_x), cx = cosf(angle_x);
    float sy = sinf(angle_y), cy = cosf(angle_y);

    /* Rotate + translate all 8 vertices. */
    float rotated[8][3];
    float proj  [8][2];   /* screen x,y */
    for (int i = 0; i < 8; i++) {
        const float *v = CUBE_V[i];
        float x = v[0] * DEMO_CUBE_SIZE;
        float y = v[1] * DEMO_CUBE_SIZE;
        float z = v[2] * DEMO_CUBE_SIZE;

        /* Rotate around X: y' = y*cx - z*sx ; z' = y*sx + z*cx */
        float y1 = y * cx - z * sx;
        float z1 = y * sx + z * cx;
        /* Rotate around Y: x' = x*cy + z1*sy ; z' = -x*sy + z1*cy */
        float x2 = x * cy + z1 * sy;
        float z2 = -x * sy + z1 * cy;

        rotated[i][0] = x2;
        rotated[i][1] = y1;
        rotated[i][2] = z2 + DEMO_CAM_DIST;

        /* Perspective project. */
        float inv_z = 1.0f / rotated[i][2];
        proj[i][0] = DEMO_CX + rotated[i][0] * DEMO_FOCAL * inv_z;
        proj[i][1] = DEMO_CY + rotated[i][1] * DEMO_FOCAL * inv_z;
    }

    /* Clear the screen to a dark background so the cube stands out. */
    gfx_clear(GFX_DARKBLUE);

    /* Draw faces. Sort by average z (painter's algorithm) so back faces
     * are overwritten by closer ones; then back-face cull per triangle. */
    /* 12 triangles; compute centroid z for each. */
    int   order[12] = { 0,1,2,3,4,5,6,7,8,9,10,11 };
    float zmid  [12];
    for (int t = 0; t < 12; t++) {
        const int *tri = CUBE_TRI[t];
        zmid[t] = (rotated[tri[0]][2] + rotated[tri[1]][2] + rotated[tri[2]][2]) / 3.0f;
    }
    /* Simple insertion sort, far -> near (descending z; larger z = farther
     * because +Z is "into the screen" after the camera translate). */
    for (int i = 1; i < 12; i++) {
        int   key_o = order[i];
        float key_z = zmid[i];
        int   j = i - 1;
        while (j >= 0 && zmid[j] < key_z) {
            zmid  [j+1] = zmid  [j];
            order [j+1] = order [j];
            j--;
        }
        zmid  [j+1] = key_z;
        order [j+1] = key_o;
    }

    for (int idx = 0; idx < 12; idx++) {
        int t = order[idx];
        const int *tri = CUBE_TRI[t];
        const float *p0 = proj[tri[0]], *p1 = proj[tri[1]], *p2 = proj[tri[2]];

        /* Signed area (screen space). CCW > 0 = facing camera. */
        float area = (p1[0] - p0[0]) * (p2[1] - p0[1])
                   - (p2[0] - p0[0]) * (p1[1] - p0[1]);
        if (area <= 0.0f) continue;   /* back-face cull */

        /* Face normal = cross(v1-v0, v2-v0) in *rotated* (world) space. */
        float e01[3], e02[3], n[3];
        vsub(e01, rotated[tri[1]], rotated[tri[0]]);
        vsub(e02, rotated[tri[2]], rotated[tri[0]]);
        vcross(n, e01, e02);
        float nl = vlen(n);
        if (nl < 1e-6f) continue;
        n[0] /= nl; n[1] /= nl; n[2] /= nl;

        /* Lambert: -dot(n, light_dir) because light points "toward" the
         * surface; flip sign so a face whose normal opposes the light is lit. */
        float lambert = -(n[0]*LIGHT_DIR[0] + n[1]*LIGHT_DIR[1] + n[2]*LIGHT_DIR[2]);

        int face_idx = t / 2;   /* 0..5 -> FACE_COLORS */
        gfx_color_t col = shade(FACE_COLORS[face_idx], lambert);

        gfx_fill_triangle((int)p0[0], (int)p0[1],
                          (int)p1[0], (int)p1[1],
                          (int)p2[0], (int)p2[1], col);
    }

    /* Draw all 12 edges as thick white lines. We draw every edge (even ones
     * on the back of the cube) because the wireframe reads better on a
     * small 240x320 panel; the filled faces already give depth cueing. */
    for (int e = 0; e < 12; e++) {
        const int *edge = CUBE_EDGE[e];
        gfx_draw_line_thick(
            (int)proj[edge[0]][0], (int)proj[edge[0]][1],
            (int)proj[edge[1]][0], (int)proj[edge[1]][1],
            2, GFX_WHITE);
    }
}

/* ----- Step driver ------------------------------------------------------- */
static float g_angle_x = 0.0f;
static float g_angle_y = 0.0f;

void cube_demo_step(void)
{
    const float d_ax = 0.035f;   /* ~2 deg/frame */
    const float d_ay = 0.052f;   /* ~3 deg/frame */
    cube_demo_render(g_angle_x, g_angle_y);
    g_angle_x += d_ax;
    g_angle_y += d_ay;
    /* Wrap to keep angles bounded (not strictly needed, keeps sin/cos fast). */
    if (g_angle_x >  6.2831853f) g_angle_x -= 6.2831853f;
    if (g_angle_y >  6.2831853f) g_angle_y -= 6.2831853f;
}
