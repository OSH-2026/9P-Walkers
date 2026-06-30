#include "lua_render_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "mini9p_protocol.h"
#include "pwos_coordinator_runtime.h"
#include "pwos_job_protocol.h"

#define PWOS_LUA_RENDER_TASK_STACK 16384u
#define PWOS_LUA_RENDER_TASK_PRIORITY 4u
#define PWOS_LUA_RENDER_DEADLINE_MS 3000u
#define PWOS_LUA_DISPLAY_TILE_MAX 128u

extern const uint8_t whitted_scheduler_lua_start[]
    asm("_binary_whitted_scheduler_lua_start");
extern const uint8_t whitted_scheduler_lua_end[]
    asm("_binary_whitted_scheduler_lua_end");

static const char *TAG = "pwos_lua_render";
static TaskHandle_t g_render_task;

static void *lua_alloc(void *ctx, void *pointer, size_t old_size, size_t new_size)
{
    (void)ctx;
    (void)old_size;
    if (new_size == 0u) {
        heap_caps_free(pointer);
        return NULL;
    }
    return heap_caps_realloc(pointer, new_size, MALLOC_CAP_8BIT);
}

static int lua_panic(lua_State *state)
{
    ESP_LOGE(TAG, "panic: %s", lua_tostring(state, -1));
    return 0;
}

static int push_error(lua_State *state, const char *kind, int code)
{
    lua_pushnil(state);
    lua_pushstring(state, kind);
    lua_pushinteger(state, (lua_Integer)code);
    return 3;
}

static int l_host_sleep(lua_State *state)
{
    lua_Integer milliseconds = luaL_checkinteger(state, 1);

    if (milliseconds < 0 || milliseconds > 60000) {
        return luaL_error(state, "sleep milliseconds out of range");
    }
    vTaskDelay(pdMS_TO_TICKS((uint32_t)milliseconds));
    return 0;
}

static int l_host_log(lua_State *state)
{
    const char *message = luaL_checkstring(state, 1);

    ESP_LOGI(TAG, "%s", message);
    return 0;
}

static int l_cluster_nodes(lua_State *state)
{
    size_t route_index;
    lua_Integer output_index = 1;

    lua_createtable(state, 4, 0);
    for (route_index = 0u; route_index < PWOS_CLUSTER_VFS_MAX_ROUTES; ++route_index) {
        pwos_cluster_vfs_route_t route;

        if (pwos_coordinator_runtime_get_route(route_index, &route) != 0 ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_EMPTY ||
            route.state == PWOS_CLUSTER_VFS_ROUTE_OFFLINE) {
            continue;
        }
        lua_pushstring(state, route.target);
        lua_rawseti(state, -2, output_index++);
    }
    return 1;
}

static int l_job_submit(lua_State *state)
{
    const char *target = luaL_checkstring(state, 1);
    size_t input_len = 0u;
    const char *input = luaL_checklstring(state, 2, &input_len);
    uint32_t host_job_id = 0u;
    uint16_t remote_status = PWOS_JOB_STATUS_OK;
    int rc;

    if (input_len == 0u || input_len > PWOS_JOB_MAX_PAYLOAD_LEN) {
        return luaL_error(state, "job payload length out of range");
    }
    rc = pwos_coordinator_runtime_job_submit(
        target,
        PWOS_JOB_KERNEL_RAYTRACE_TILE,
        (const uint8_t *)input,
        (uint16_t)input_len,
        PWOS_LUA_RENDER_DEADLINE_MS,
        &host_job_id,
        &remote_status);
    if (rc != 0) return push_error(state, "transport", rc);
    if (remote_status != PWOS_JOB_STATUS_OK) {
        return push_error(state, pwos_job_status_name(remote_status), remote_status);
    }
    lua_pushinteger(state, (lua_Integer)host_job_id);
    return 1;
}

static int l_job_result(lua_State *state)
{
    lua_Integer id = luaL_checkinteger(state, 1);
    uint8_t result[PWOS_JOB_MAX_PAYLOAD_LEN];
    uint16_t result_len = sizeof(result);
    uint16_t remote_status = PWOS_JOB_STATUS_OK;
    pwos_job_entry_t entry;
    int rc;

    if (id <= 0) return luaL_error(state, "invalid job id");
    memset(&entry, 0, sizeof(entry));
    rc = pwos_coordinator_runtime_job_result(
        (uint32_t)id,
        PWOS_LUA_RENDER_DEADLINE_MS,
        result,
        &result_len,
        &entry,
        &remote_status);
    if (rc != 0) return push_error(state, "transport", rc);
    if (remote_status == PWOS_JOB_STATUS_OK) {
        lua_pushlstring(state, (const char *)result, result_len);
        lua_pushstring(state, "done");
        lua_pushinteger(state, (lua_Integer)entry.progress_permille);
        return 3;
    }
    lua_pushnil(state);
    lua_pushstring(state, pwos_job_status_name(remote_status));
    lua_pushinteger(state, (lua_Integer)entry.progress_permille);
    return 3;
}

static int l_display_probe(lua_State *state)
{
    const char *target = luaL_checkstring(state, 1);
    char path[48];
    uint8_t status[96];
    uint16_t status_len = sizeof(status);
    int written;
    int rc;

    written = snprintf(path, sizeof(path), "/%s/display/status", target);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return luaL_error(state, "target name too long");
    }
    rc = pwos_coordinator_runtime_read_path(
        path, status, &status_len, PWOS_LUA_RENDER_DEADLINE_MS);
    lua_pushboolean(state, rc == 0 && status_len > 0u);
    return 1;
}

static int l_display_blit(lua_State *state)
{
    const char *target = luaL_checkstring(state, 1);
    size_t tile_len = 0u;
    const char *tile = luaL_checklstring(state, 2, &tile_len);
    char path[48];
    uint16_t written_count = 0u;
    int path_len;
    int rc;

    if (tile_len == 0u || tile_len > PWOS_LUA_DISPLAY_TILE_MAX) {
        return luaL_error(state, "display tile length out of range");
    }
    path_len = snprintf(path, sizeof(path), "/%s/display/tile", target);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
        return luaL_error(state, "target name too long");
    }
    rc = pwos_coordinator_runtime_write_path(
        path,
        (const uint8_t *)tile,
        (uint16_t)tile_len,
        &written_count,
        PWOS_LUA_RENDER_DEADLINE_MS);
    if (rc != 0 || written_count != tile_len) {
        return push_error(state, "write", rc != 0 ? rc : -(int)M9P_ERR_EIO);
    }
    lua_pushboolean(state, 1);
    return 1;
}

static void register_bindings(lua_State *state)
{
    static const luaL_Reg host_functions[] = {
        {"sleep", l_host_sleep},
        {"log", l_host_log},
        {NULL, NULL},
    };
    static const luaL_Reg cluster_functions[] = {
        {"nodes", l_cluster_nodes},
        {NULL, NULL},
    };
    static const luaL_Reg job_functions[] = {
        {"submit", l_job_submit},
        {"result", l_job_result},
        {NULL, NULL},
    };
    static const luaL_Reg display_functions[] = {
        {"probe", l_display_probe},
        {"blit", l_display_blit},
        {NULL, NULL},
    };

    luaL_newlib(state, host_functions);
    lua_setglobal(state, "host");
    luaL_newlib(state, cluster_functions);
    lua_setglobal(state, "cluster");
    luaL_newlib(state, job_functions);
    lua_setglobal(state, "job");
    luaL_newlib(state, display_functions);
    lua_setglobal(state, "display");
}

static void render_task(void *argument)
{
    const char *script = (const char *)whitted_scheduler_lua_start;
    size_t script_len = (size_t)(whitted_scheduler_lua_end - whitted_scheduler_lua_start);
    lua_State *state;
    int rc;

    (void)argument;
    state = lua_newstate(lua_alloc, NULL, esp_random());
    if (state == NULL) {
        ESP_LOGE(TAG, "cannot allocate Lua state");
        g_render_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    lua_atpanic(state, lua_panic);
    luaL_openlibs(state);
    register_bindings(state);
    rc = luaL_loadbuffer(state, script, script_len, "whitted_scheduler");
    if (rc == LUA_OK) rc = lua_pcall(state, 0, 0, 0);
    if (rc != LUA_OK) {
        ESP_LOGE(TAG, "scheduler stopped: %s", lua_tostring(state, -1));
    }
    lua_close(state);
    g_render_task = NULL;
    vTaskDelete(NULL);
}

int pwos_lua_render_runtime_start(void)
{
    BaseType_t created;

    if (g_render_task != NULL) return 0;
    created = xTaskCreate(
        render_task,
        "lua_render",
        PWOS_LUA_RENDER_TASK_STACK,
        NULL,
        PWOS_LUA_RENDER_TASK_PRIORITY,
        &g_render_task);
    return created == pdPASS ? 0 : -1;
}
