#include "pwos_smallpt.h"

#include <math.h>
#include <string.h>

/* ---- 向量运算 ---- */

typedef struct { float x, y, z; } vec3_t;
typedef struct { vec3_t origin, direction; } ray_t;

typedef enum {
    MATERIAL_DIFFUSE = 0,
    MATERIAL_MIRROR,
    MATERIAL_GLASS,
} material_t;

typedef struct {
    vec3_t center;
    float radius;
    vec3_t color;
    material_t material;
} sphere_t;

typedef struct {
    uint8_t axis;
    float pos;
    float normal_sign;
    vec3_t color;
    material_t material;
} wall_t;

/* ---- 场景参数 ---- */

/* 光源是虚拟球（不参与场景求交，仅用于直接光照采样） */
static const vec3_t k_light_center   = {0.5f, 0.92f, 0.5f};
static const float   k_light_radius  = 0.06f;
static const vec3_t k_light_emit    = {8.0f, 8.0f, 7.0f};

static const sphere_t k_spheres[] = {
    {{0.27f, 0.27f, 0.62f}, 0.27f, {0.95f, 0.95f, 0.95f}, MATERIAL_MIRROR},
    {{0.73f, 0.27f, 0.38f}, 0.27f, {0.99f, 0.99f, 0.99f}, MATERIAL_GLASS},
};

static const wall_t k_walls[] = {
    {0, 0.0f, +1.0f, {0.75f, 0.25f, 0.25f}, MATERIAL_DIFFUSE},
    {0, 1.0f, -1.0f, {0.25f, 0.75f, 0.25f}, MATERIAL_DIFFUSE},
    {1, 0.0f, +1.0f, {0.73f, 0.73f, 0.73f}, MATERIAL_DIFFUSE},
    {1, 1.0f, -1.0f, {0.73f, 0.73f, 0.73f}, MATERIAL_DIFFUSE},
    {2, 1.0f, -1.0f, {0.73f, 0.73f, 0.73f}, MATERIAL_DIFFUSE},
};

static const float PI = 3.14159265358979f;

static inline vec3_t v3(float x, float y, float z) { vec3_t v = {x, y, z}; return v; }
static inline vec3_t vadd(vec3_t a, vec3_t b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline vec3_t vsub(vec3_t a, vec3_t b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline vec3_t vscale(vec3_t v, float s) { return v3(v.x*s, v.y*s, v.z*s); }
static inline vec3_t vmul(vec3_t a, vec3_t b) { return v3(a.x*b.x, a.y*b.y, a.z*b.z); }
static inline float vdot(vec3_t a, vec3_t b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline vec3_t vcross(vec3_t a, vec3_t b) {
    return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline vec3_t vnorm(vec3_t v) {
    float len2 = vdot(v, v);
    if (len2 < 1.0e-12f) return v3(0, 0, 0);
    return vscale(v, 1.0f / sqrtf(len2));
}
static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/* ---- xorshift32 ---- */

static inline uint32_t rng_u32(uint32_t *s) {
    uint32_t v = *s;
    if (v == 0) v = 0x6D2B79F5u;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    *s = v;
    return v;
}
static inline float rng_unit(uint32_t *s) {
    return (float)(rng_u32(s) & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

/* ---- 求交 ---- */

typedef struct {
    float distance;
    vec3_t point;
    vec3_t normal;
    vec3_t color;
    material_t material;
} hit_t;

static int isect_sphere(const ray_t *r, const sphere_t *sp, float *out_t) {
    vec3_t oc = vsub(sp->center, r->origin);
    float proj = vdot(oc, r->direction);
    float d2 = vdot(oc, oc) - proj * proj;
    float r2 = sp->radius * sp->radius;
    if (d2 > r2) return 0;
    float root = sqrtf(r2 - d2);
    float t = proj - root;
    if (t < 1.0e-3f) t = proj + root;
    if (t < 1.0e-3f) return 0;
    *out_t = t;
    return 1;
}

static int isect_wall(const ray_t *r, const wall_t *w, float *out_t) {
    float dir_a, org_a;
    if (w->axis == 0)      { dir_a = r->direction.x; org_a = r->origin.x; }
    else if (w->axis == 1) { dir_a = r->direction.y; org_a = r->origin.y; }
    else                   { dir_a = r->direction.z; org_a = r->origin.z; }
    if (dir_a > -1.0e-6f && dir_a < 1.0e-6f) return 0;
    float t = (w->pos - org_a) / dir_a;
    if (t < 1.0e-3f) return 0;
    vec3_t p = vadd(r->origin, vscale(r->direction, t));
    float p_a1, p_a2;
    int a1 = (w->axis + 1) % 3;
    int a2 = (w->axis + 2) % 3;
    if (a1 == 0) p_a1 = p.x; else if (a1 == 1) p_a1 = p.y; else p_a1 = p.z;
    if (a2 == 0) p_a2 = p.x; else if (a2 == 1) p_a2 = p.y; else p_a2 = p.z;
    if (p_a1 < 0.0f || p_a1 > 1.0f || p_a2 < 0.0f || p_a2 > 1.0f) return 0;
    *out_t = t;
    return 1;
}

static int intersect_scene(const ray_t *r, hit_t *out) {
    float nearest = 1.0e30f;
    int found = 0;
    size_t i;
    for (i = 0; i < sizeof(k_spheres) / sizeof(k_spheres[0]); ++i) {
        float t;
        if (isect_sphere(r, &k_spheres[i], &t) && t < nearest) {
            nearest = t;
            out->material = k_spheres[i].material;
            out->color = k_spheres[i].color;
            out->point = vadd(r->origin, vscale(r->direction, t));
            out->normal = vnorm(vsub(out->point, k_spheres[i].center));
            found = 1;
        }
    }
    for (i = 0; i < sizeof(k_walls) / sizeof(k_walls[0]); ++i) {
        float t;
        if (isect_wall(r, &k_walls[i], &t) && t < nearest) {
            nearest = t;
            out->material = k_walls[i].material;
            out->color = k_walls[i].color;
            out->point = vadd(r->origin, vscale(r->direction, t));
            if (k_walls[i].axis == 0)
                out->normal = v3(k_walls[i].normal_sign, 0, 0);
            else if (k_walls[i].axis == 1)
                out->normal = v3(0, k_walls[i].normal_sign, 0);
            else
                out->normal = v3(0, 0, k_walls[i].normal_sign);
            found = 1;
        }
    }
    out->distance = nearest;
    return found;
}

static int occluded(vec3_t origin, vec3_t dir, float max_t) {
    ray_t r = {origin, dir};
    hit_t h;
    if (!intersect_scene(&r, &h)) return 0;
    return h.distance < max_t;
}

/* ---- 直接光照（Next Event Estimation） ---- */

/* 从表面点向光源球做直接光照采样，返回辐照度 */
static vec3_t direct_lighting(vec3_t point, vec3_t normal, vec3_t albedo) {
    vec3_t to_c = vsub(k_light_center, point);
    float dist = sqrtf(vdot(to_c, to_c));
    if (dist < 1e-4f) return v3(0, 0, 0);

    vec3_t ldir = vscale(to_c, 1.0f / dist);
    float ndotl = vdot(normal, ldir);
    if (ndotl <= 0) return v3(0, 0, 0);

    /* 阴影射线：偏移起点避免自交，终点到光源球表面 */
    vec3_t shadow_origin = vadd(point, vscale(normal, 0.002f));
    if (occluded(shadow_origin, ldir, dist - k_light_radius))
        return v3(0, 0, 0);

    /* 光源球对的立体角 */
    float sin_theta = k_light_radius / dist;
    float cos_theta = sqrtf(1.0f - sin_theta * sin_theta);
    float solid_angle = 2.0f * PI * (1.0f - cos_theta);

    /* Lambert BRDF: albedo / PI, 乘以立体角和余弦项 */
    vec3_t contrib = vscale(vmul(albedo, k_light_emit),
                            ndotl * solid_angle / PI);
    return contrib;
}

/* ---- 余弦加权半球采样 ---- */

static vec3_t cosine_hemisphere(vec3_t n, uint32_t *rng) {
    float r1 = 2.0f * PI * rng_unit(rng);
    float r2 = rng_unit(rng);
    float sx = cosf(r1) * sqrtf(r2);
    float sy = sinf(r1) * sqrtf(r2);
    float sz = sqrtf(1.0f - r2);
    vec3_t u;
    if (fabsf(n.x) > 0.1f)
        u = vnorm(vcross(v3(0, 1, 0), n));
    else
        u = vnorm(vcross(v3(1, 0, 0), n));
    vec3_t v = vcross(n, u);
    return vnorm(vadd(vadd(vscale(u, sx), vscale(v, sy)), vscale(n, sz)));
}

/* ---- 路径追踪（迭代式，含 NEE） ---- */

static vec3_t trace_path(ray_t ray, uint8_t max_depth, uint32_t *rng) {
    vec3_t throughput = {1, 1, 1};
    vec3_t radiance = {0, 0, 0};
    uint8_t depth;

    for (depth = 0; depth < max_depth; ++depth) {
        hit_t h;
        if (!intersect_scene(&ray, &h)) break;

        /* NEE 只用于漫反射表面：镜面和玻璃的光照来自随机弹射 */
        if (h.material == MATERIAL_DIFFUSE) {
            vec3_t direct = direct_lighting(h.point, h.normal, h.color);
            radiance = vadd(radiance, vmul(throughput, direct));
        }

        if (h.material == MATERIAL_DIFFUSE) {
            ray.direction = cosine_hemisphere(h.normal, rng);
            ray.origin = vadd(h.point, vscale(h.normal, 0.002f));
            throughput = vmul(throughput, h.color);
            if (depth >= 2 && rng_unit(rng) < 0.5f) break;
            continue;
        }

        if (h.material == MATERIAL_MIRROR) {
            ray.direction = vnorm(vsub(ray.direction,
                vscale(h.normal, 2.0f * vdot(ray.direction, h.normal))));
            ray.origin = vadd(h.point, vscale(h.normal, 0.002f));
            throughput = vmul(throughput, h.color);
            continue;
        }

        /* 玻璃 */
        {
            vec3_t n = h.normal;
            float cosi = -vdot(ray.direction, n);
            float etai = 1.0f, etat = 1.5f;
            if (cosi < 0) { cosi = -cosi; n = vscale(n, -1); etai = 1.5f; etat = 1.0f; }
            float eta = etai / etat;
            float sin2 = eta * eta * (1 - cosi * cosi);
            float fres = 0.04f + 0.96f *
                (1 - cosi) * (1 - cosi) * (1 - cosi) * (1 - cosi) * (1 - cosi);
            if (sin2 > 1.0f || rng_unit(rng) < fres) {
                ray.direction = vnorm(vsub(ray.direction,
                    vscale(n, 2.0f * vdot(ray.direction, n))));
            } else {
                ray.direction = vnorm(vadd(vscale(ray.direction, eta),
                    vscale(n, eta * cosi - sqrtf(1 - sin2))));
            }
            ray.origin = vadd(h.point, vscale(ray.direction, 0.002f));
            throughput = vmul(throughput, h.color);
        }
    }
    return radiance;
}

/* ---- 协议编解码 ---- */

static uint16_t get_le16(const uint8_t *d) {
    return (uint16_t)(d[0] | ((uint16_t)d[1] << 8));
}
static uint32_t get_le32(const uint8_t *d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
           ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}
static void put_le16(uint8_t *d, uint16_t v) {
    d[0] = (uint8_t)(v & 0xFF);
    d[1] = (uint8_t)(v >> 8);
}

int pwos_render_decode_request(const uint8_t *data, size_t len, pwos_render_request_t *out) {
    pwos_render_request_t r;
    if (data == NULL || out == NULL || len != PWOS_RENDER_REQUEST_LEN ||
        data[0] != PWOS_RENDER_PROTOCOL_VERSION) return -1;
    memset(&r, 0, sizeof(r));
    r.scene_id = data[1];
    r.tile_x = get_le16(data + 2);
    r.tile_y = get_le16(data + 4);
    r.tile_w = data[6];
    r.tile_h = data[7];
    r.image_w = get_le16(data + 8);
    r.image_h = get_le16(data + 10);
    r.samples = data[12];
    r.max_depth = data[13];
    r.frame_id = get_le16(data + 14);
    r.seed = get_le32(data + 16);
    r.camera_phase = get_le16(data + 20);
    if (r.scene_id != PWOS_RENDER_SCENE_WHITTED ||
        r.tile_w == 0 || r.tile_w > PWOS_RENDER_TILE_MAX_WIDTH ||
        r.tile_h == 0 || r.tile_h > PWOS_RENDER_TILE_MAX_HEIGHT ||
        r.image_w == 0 || r.image_h == 0 ||
        (uint16_t)r.tile_x + r.tile_w > r.image_w ||
        (uint16_t)r.tile_y + r.tile_h > r.image_h ||
        r.samples == 0 || r.samples > 8u ||
        r.max_depth == 0 || r.max_depth > 5u) return -1;
    *out = r;
    return 0;
}

uint16_t pwos_render_result_len(const pwos_render_request_t *req) {
    if (req == NULL) return 0;
    return (uint16_t)(PWOS_RENDER_RESULT_HEADER_LEN + (uint16_t)req->tile_w * req->tile_h * 2u);
}

void pwos_render_write_result_header(const pwos_render_request_t *req, uint8_t *result) {
    if (req == NULL || result == NULL) return;
    result[0] = PWOS_RENDER_PROTOCOL_VERSION;
    result[1] = req->scene_id;
    put_le16(result + 2, req->tile_x);
    put_le16(result + 4, req->tile_y);
    result[6] = req->tile_w;
    result[7] = req->tile_h;
    put_le16(result + 8, req->image_w);
    put_le16(result + 10, req->image_h);
    put_le16(result + 12, req->frame_id);
    result[14] = PWOS_RENDER_FORMAT_RGB565_LE;
    result[15] = 0;
}

/* ---- 像素渲染 ---- */

uint16_t pwos_smallpt_render_pixel(const pwos_render_request_t *req, uint32_t idx) {
    uint32_t lx = idx % req->tile_w;
    uint32_t ly = idx / req->tile_w;
    uint32_t px = (uint32_t)req->tile_x + lx;
    uint32_t py = (uint32_t)req->tile_y + ly;
    float ang = (float)(req->camera_phase % 360u) * (PI / 180.0f);

    vec3_t eye = v3(0.5f + 0.04f * sinf(ang), 0.5f, 0.0f - 1.65f + 0.02f * cosf(ang));
    vec3_t target = v3(0.5f, 0.5f, 0.5f);
    vec3_t fwd = vnorm(vsub(target, eye));
    vec3_t right = vnorm(vcross(fwd, v3(0, 1, 0)));
    vec3_t up = vcross(right, fwd);
    float aspect = (float)req->image_w / (float)req->image_h;
    float fov_scale = 0.35f;

    uint32_t rng = req->seed ^ (px * 0x9E3779B9u) ^ (py * 0x85EBCA6Bu) ^
                   ((uint32_t)req->frame_id << 16);
    vec3_t acc = {0, 0, 0};
    uint8_t s;

    for (s = 0; s < req->samples; ++s) {
        float jx = rng_unit(&rng);
        float jy = rng_unit(&rng);
        float sx = (((float)px + jx) / req->image_w * 2.0f - 1.0f) * aspect * fov_scale;
        float sy = (1.0f - ((float)py + jy) / req->image_h * 2.0f) * fov_scale;
        ray_t ray;
        ray.origin = eye;
        ray.direction = vnorm(vadd(fwd, vadd(vscale(right, sx), vscale(up, sy))));
        vec3_t sample_rad = trace_path(ray, req->max_depth, &rng);
        /* 限制单样本亮度，防止 firefly 噪点 */
        sample_rad = v3(clamp01(sample_rad.x), clamp01(sample_rad.y), clamp01(sample_rad.z));
        acc = vadd(acc, sample_rad);
    }

    acc = vscale(acc, 1.0f / req->samples);
    uint8_t r = (uint8_t)(sqrtf(clamp01(acc.x)) * 255.0f + 0.5f);
    uint8_t g = (uint8_t)(sqrtf(clamp01(acc.y)) * 255.0f + 0.5f);
    uint8_t b = (uint8_t)(sqrtf(clamp01(acc.z)) * 255.0f + 0.5f);
    return (uint16_t)((((uint16_t)r & 0xF8) << 8) |
                       (((uint16_t)g & 0xFC) << 3) |
                       ((uint16_t)b >> 3));
}
