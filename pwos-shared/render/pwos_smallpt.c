#include "pwos_smallpt.h"

#include <math.h>
#include <string.h>

typedef struct {
    float x;
    float y;
    float z;
} vec3_t;

typedef struct {
    vec3_t origin;
    vec3_t direction;
} ray_t;

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
    vec3_t point;
    vec3_t normal;
    vec3_t color;
    material_t material;
    uint8_t axis;        /* 0=X plane, 1=Y plane, 2=Z plane */
    float min_u, max_u;  /* 另两个轴的边界 */
    float min_v, max_v;
} plane_t;

typedef struct {
    float distance;
    vec3_t point;
    vec3_t normal;
    vec3_t color;
    material_t material;
} hit_t;

/* Cornell Box 中的三个经典球体。 */
static const sphere_t k_spheres[] = {
    {{-1.05f, -0.22f, -0.60f}, 0.78f, {0.82f, 0.16f, 0.11f}, MATERIAL_DIFFUSE},
    {{ 0.82f, -0.38f, -0.80f}, 0.62f, {0.92f, 0.95f, 1.00f}, MATERIAL_MIRROR},
    {{ 0.05f, -0.10f, -1.80f}, 0.85f, {0.93f, 0.98f, 1.00f}, MATERIAL_GLASS},
};

/* 经典 Cornell Box：左红、右绿、其余白墙，无正面墙方便相机观察。 */
static const plane_t k_planes[] = {
    /* floor   */
    {{0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.725f, 0.71f, 0.68f}, MATERIAL_DIFFUSE, 1,
     -2.0f, 2.0f, -4.0f, 0.5f},
    /* ceiling */
    {{0.0f,  2.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.725f, 0.735f, 0.73f}, MATERIAL_DIFFUSE, 1,
     -2.0f, 2.0f, -4.0f, 0.5f},
    /* back    */
    {{0.0f, 0.0f, -4.0f}, {0.0f, 0.0f, 1.0f}, {0.725f, 0.735f, 0.73f}, MATERIAL_DIFFUSE, 2,
     -2.0f, 2.0f, -1.0f, 2.0f},
    /* left red */
    {{-2.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.63f, 0.065f, 0.05f}, MATERIAL_DIFFUSE, 0,
     -1.0f, 2.0f, -4.0f, 0.5f},
    /* right green */
    {{ 2.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.14f, 0.45f, 0.091f}, MATERIAL_DIFFUSE, 0,
     -1.0f, 2.0f, -4.0f, 0.5f},
};

static vec3_t vec3(float x, float y, float z)
{
    vec3_t value = {x, y, z};
    return value;
}

static vec3_t add(vec3_t a, vec3_t b)
{
    return vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static vec3_t sub(vec3_t a, vec3_t b)
{
    return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static vec3_t scale(vec3_t value, float factor)
{
    return vec3(value.x * factor, value.y * factor, value.z * factor);
}

static vec3_t multiply(vec3_t a, vec3_t b)
{
    return vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}

static float dot(vec3_t a, vec3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3_t cross(vec3_t a, vec3_t b)
{
    return vec3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

static vec3_t normalize(vec3_t value)
{
    float length = sqrtf(dot(value, value));
    return length > 1.0e-8f ? scale(value, 1.0f / length) : vec3(0.0f, 0.0f, 0.0f);
}

static float clamp01(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static uint16_t get_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t get_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static void put_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)(value >> 8);
}

static uint32_t random_u32(uint32_t *state)
{
    uint32_t value = *state;

    if (value == 0u) value = 0x6D2B79F5u;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static float random_unit(uint32_t *state)
{
    return (float)(random_u32(state) & 0x00FFFFFFu) / 16777216.0f;
}

static int intersect_sphere(const ray_t *ray, const sphere_t *sphere, float *out_distance)
{
    vec3_t offset = sub(sphere->center, ray->origin);
    float projection = dot(offset, ray->direction);
    float discriminant = sphere->radius * sphere->radius -
        dot(offset, offset) + projection * projection;
    float root;
    float distance;

    if (discriminant < 0.0f) return 0;
    root = sqrtf(discriminant);
    distance = projection - root;
    if (distance < 1.0e-3f) distance = projection + root;
    if (distance < 1.0e-3f) return 0;
    *out_distance = distance;
    return 1;
}

static int intersect_plane(const ray_t *ray, const plane_t *plane, float *out_distance)
{
    float denom = dot(ray->direction, plane->normal);
    float distance;
    vec3_t hit_point;
    float u, v;

    if (denom > -1.0e-5f && denom < 1.0e-5f) return 0;
    distance = dot(sub(plane->point, ray->origin), plane->normal) / denom;
    if (distance < 1.0e-3f) return 0;
    hit_point = add(ray->origin, scale(ray->direction, distance));
    if (plane->axis == 0) {      /* X plane: 检查 y, z */
        u = hit_point.y;
        v = hit_point.z;
    } else if (plane->axis == 1) { /* Y plane: 检查 x, z */
        u = hit_point.x;
        v = hit_point.z;
    } else {                     /* Z plane: 检查 x, y */
        u = hit_point.x;
        v = hit_point.y;
    }
    if (u < plane->min_u || u > plane->max_u ||
        v < plane->min_v || v > plane->max_v) {
        return 0;
    }
    *out_distance = distance;
    return 1;
}

static int intersect_scene(const ray_t *ray, hit_t *out_hit)
{
    float nearest = 1.0e30f;
    int found = 0;
    size_t i;

    for (i = 0u; i < sizeof(k_spheres) / sizeof(k_spheres[0]); ++i) {
        float distance;

        if (intersect_sphere(ray, &k_spheres[i], &distance) && distance < nearest) {
            nearest = distance;
            out_hit->material = k_spheres[i].material;
            out_hit->color = k_spheres[i].color;
            out_hit->point = add(ray->origin, scale(ray->direction, distance));
            out_hit->normal = normalize(sub(out_hit->point, k_spheres[i].center));
            found = 1;
        }
    }

    for (i = 0u; i < sizeof(k_planes) / sizeof(k_planes[0]); ++i) {
        const plane_t *plane = &k_planes[i];
        float distance;

        if (intersect_plane(ray, plane, &distance) && distance < nearest) {
            nearest = distance;
            out_hit->material = plane->material;
            out_hit->color = plane->color;
            out_hit->point = add(ray->origin, scale(ray->direction, distance));
            out_hit->normal = plane->normal;
            found = 1;
        }
    }

    out_hit->distance = nearest;
    return found;
}

static int occluded(vec3_t origin, vec3_t direction, float max_distance)
{
    ray_t ray = {origin, direction};
    hit_t hit;

    return intersect_scene(&ray, &hit) && hit.distance < max_distance;
}

static vec3_t sky_color(vec3_t direction)
{
    float blend = clamp01(0.5f * (direction.y + 1.0f));
    return add(scale(vec3(0.08f, 0.10f, 0.16f), 1.0f - blend),
               scale(vec3(0.46f, 0.62f, 0.82f), blend));
}

static vec3_t trace_path(ray_t ray, uint8_t max_depth, uint32_t *rng)
{
    const vec3_t light_position = {0.0f, 1.8f, -1.5f};
    const vec3_t light_color = {1.0f, 0.90f, 0.76f};
    vec3_t throughput = {1.0f, 1.0f, 1.0f};
    vec3_t radiance = {0.0f, 0.0f, 0.0f};
    uint8_t depth;

    for (depth = 0u; depth < max_depth; ++depth) {
        hit_t hit;
        vec3_t to_light;
        vec3_t light_dir;
        float light_distance;
        float diffuse;

        if (!intersect_scene(&ray, &hit)) {
            radiance = add(radiance, multiply(throughput, sky_color(ray.direction)));
            break;
        }

        to_light = sub(light_position, hit.point);
        light_distance = sqrtf(dot(to_light, to_light));
        light_dir = scale(to_light, 1.0f / light_distance);
        diffuse = dot(hit.normal, light_dir);
        radiance = add(radiance, scale(multiply(throughput, hit.color), 0.035f));
        if (diffuse > 0.0f &&
            !occluded(add(hit.point, scale(hit.normal, 0.002f)), light_dir, light_distance)) {
            float attenuation = 2.5f / (light_distance * light_distance);
            vec3_t direct = scale(multiply(hit.color, light_color), diffuse * attenuation);
            radiance = add(radiance, multiply(throughput, direct));
        }

        if (hit.material == MATERIAL_DIFFUSE) {
            break;
        }

        if (hit.material == MATERIAL_MIRROR) {
            ray.direction = normalize(sub(
                ray.direction, scale(hit.normal, 2.0f * dot(ray.direction, hit.normal))));
            ray.origin = add(hit.point, scale(hit.normal, 0.002f));
            throughput = scale(multiply(throughput, hit.color), 0.88f);
            continue;
        }

        /* 小型 Schlick Fresnel + 折射实现，迭代循环避免 MCU 上的递归栈。 */
        {
            vec3_t normal = hit.normal;
            float eta_i = 1.0f;
            float eta_t = 1.5f;
            float cosine = -dot(ray.direction, normal);
            float eta;
            float sine2;
            float fresnel;

            if (cosine < 0.0f) {
                cosine = -cosine;
                normal = scale(normal, -1.0f);
                eta_i = 1.5f;
                eta_t = 1.0f;
            }
            eta = eta_i / eta_t;
            sine2 = eta * eta * (1.0f - cosine * cosine);
            fresnel = 0.04f + 0.96f *
                (1.0f - cosine) * (1.0f - cosine) *
                (1.0f - cosine) * (1.0f - cosine) * (1.0f - cosine);
            if (sine2 > 1.0f || random_unit(rng) < fresnel) {
                ray.direction = normalize(sub(
                    ray.direction, scale(normal, 2.0f * dot(ray.direction, normal))));
            } else {
                ray.direction = normalize(add(
                    scale(ray.direction, eta),
                    scale(normal, eta * cosine - sqrtf(1.0f - sine2))));
            }
            ray.origin = add(hit.point, scale(ray.direction, 0.002f));
            throughput = scale(multiply(throughput, hit.color), 0.95f);
        }
    }
    return radiance;
}

int pwos_render_decode_request(
    const uint8_t *data,
    size_t len,
    pwos_render_request_t *out_request)
{
    pwos_render_request_t request;

    if (data == NULL || out_request == NULL || len != PWOS_RENDER_REQUEST_LEN ||
        data[0] != PWOS_RENDER_PROTOCOL_VERSION) {
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.scene_id = data[1];
    request.tile_x = data[2];
    request.tile_y = data[3];
    request.tile_w = data[4];
    request.tile_h = data[5];
    request.image_w = data[6];
    request.image_h = data[7];
    request.samples = data[8];
    request.max_depth = data[9];
    request.frame_id = get_le16(data + 10u);
    request.seed = get_le32(data + 12u);
    request.camera_phase = get_le16(data + 16u);
    if (request.scene_id != PWOS_RENDER_SCENE_WHITTED ||
        request.tile_w == 0u || request.tile_w > PWOS_RENDER_TILE_MAX_WIDTH ||
        request.tile_h == 0u || request.tile_h > PWOS_RENDER_TILE_MAX_HEIGHT ||
        request.image_w == 0u || request.image_h == 0u ||
        (uint16_t)request.tile_x + request.tile_w > request.image_w ||
        (uint16_t)request.tile_y + request.tile_h > request.image_h ||
        request.samples == 0u || request.samples > 8u ||
        request.max_depth == 0u || request.max_depth > 5u) {
        return -1;
    }
    *out_request = request;
    return 0;
}

uint16_t pwos_render_result_len(const pwos_render_request_t *request)
{
    if (request == NULL) return 0u;
    return (uint16_t)(PWOS_RENDER_RESULT_HEADER_LEN +
        (uint16_t)request->tile_w * request->tile_h * 2u);
}

void pwos_render_write_result_header(
    const pwos_render_request_t *request,
    uint8_t *result)
{
    if (request == NULL || result == NULL) return;
    result[0] = PWOS_RENDER_PROTOCOL_VERSION;
    result[1] = request->scene_id;
    result[2] = request->tile_x;
    result[3] = request->tile_y;
    result[4] = request->tile_w;
    result[5] = request->tile_h;
    result[6] = request->image_w;
    result[7] = request->image_h;
    put_le16(result + 8u, request->frame_id);
    result[10] = PWOS_RENDER_FORMAT_RGB565_LE;
    result[11] = 0u;
}

uint16_t pwos_smallpt_render_pixel(
    const pwos_render_request_t *request,
    uint32_t tile_pixel_index)
{
    const float pi = 3.14159265358979323846f;
    uint32_t local_x = tile_pixel_index % request->tile_w;
    uint32_t local_y = tile_pixel_index / request->tile_w;
    uint32_t pixel_x = (uint32_t)request->tile_x + local_x;
    uint32_t pixel_y = (uint32_t)request->tile_y + local_y;
    float phase = (float)(request->camera_phase % 360u) * (pi / 180.0f);
    vec3_t camera = vec3(0.55f * sinf(phase), 1.05f, 4.35f + 0.18f * cosf(phase));
    vec3_t target = vec3(0.0f, -0.05f, -0.55f);
    vec3_t forward = normalize(sub(target, camera));
    vec3_t right = normalize(cross(forward, vec3(0.0f, 1.0f, 0.0f)));
    vec3_t up = cross(right, forward);
    float aspect = (float)request->image_w / (float)request->image_h;
    uint32_t rng = request->seed ^ (pixel_x * 0x9E3779B9u) ^
        (pixel_y * 0x85EBCA6Bu) ^ ((uint32_t)request->frame_id << 16);
    vec3_t accumulated = {0.0f, 0.0f, 0.0f};
    uint8_t sample;
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    for (sample = 0u; sample < request->samples; ++sample) {
        float jitter_x = random_unit(&rng);
        float jitter_y = random_unit(&rng);
        float screen_x = (((float)pixel_x + jitter_x) / request->image_w * 2.0f - 1.0f) *
            aspect * 0.56f;
        float screen_y = (1.0f - ((float)pixel_y + jitter_y) /
            request->image_h * 2.0f) * 0.56f;
        ray_t ray;

        ray.origin = camera;
        ray.direction = normalize(add(forward, add(scale(right, screen_x), scale(up, screen_y))));
        accumulated = add(accumulated, trace_path(ray, request->max_depth, &rng));
    }

    accumulated = scale(accumulated, 1.0f / request->samples);
    /* sqrt 是便宜且稳定的近似 gamma 2.0 校正。 */
    red = (uint8_t)(sqrtf(clamp01(accumulated.x)) * 255.0f + 0.5f);
    green = (uint8_t)(sqrtf(clamp01(accumulated.y)) * 255.0f + 0.5f);
    blue = (uint8_t)(sqrtf(clamp01(accumulated.z)) * 255.0f + 0.5f);
    return (uint16_t)((((uint16_t)red & 0xF8u) << 8) |
                      (((uint16_t)green & 0xFCu) << 3) |
                      ((uint16_t)blue >> 3));
}
