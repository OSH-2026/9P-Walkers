#include "lua_bindings.h"

#include <stdio.h>
#include <stdint.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "mini9p_protocol.h"
#include "sdkconfig.h"

/**
 * @brief 
 * 
 * @param L 
 * @return int 
 */
static int host_echo(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);

    printf("[lua] %s\n", text);
    lua_pushstring(L, text);
    return 1;
}

static int host_free_heap(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return 1;
}

static int host_uptime_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int host_sleep_ms(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);

    if (ms < 0) {
        return luaL_error(L, "sleep_ms expects a non-negative value");
    }

    vTaskDelay(pdMS_TO_TICKS((TickType_t)ms));
    return 0;
}

static int host_chip(lua_State *L)
{
    esp_chip_info_t chip_info;

    esp_chip_info(&chip_info);

    /* Returning a Lua table here makes the demo script easy to read:
     * local chip = host.chip()
     */
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)chip_info.cores);
    lua_setfield(L, -2, "cores");
    lua_pushinteger(L, (lua_Integer)chip_info.revision);
    lua_setfield(L, -2, "revision");
    lua_pushstring(L, CONFIG_IDF_TARGET);
    lua_setfield(L, -2, "target");
    return 1;
}

/* m9p.* exposes a tiny slice of the protocol layer so the report can
 * show that Lua is already connected to project-specific code.
 */
static int m9p_error_name_lua(lua_State *L)
{
    lua_Integer code = luaL_checkinteger(L, 1);

    lua_pushstring(L, m9p_error_name((uint16_t)code));
    return 1;
}

static int m9p_version_lua(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)M9P_VERSION);
    return 1;
}

void pw_lua_register_bindings(lua_State *L)
{
    static const luaL_Reg host_lib[] = {
        {"echo", host_echo},
        {"free_heap", host_free_heap},
        {"uptime_ms", host_uptime_ms},
        {"sleep_ms", host_sleep_ms},
        {"chip", host_chip},
        {NULL, NULL},
    };
    static const luaL_Reg m9p_lib[] = {
        {"error_name", m9p_error_name_lua},
        {"version", m9p_version_lua},
        {NULL, NULL},
    };

    /* Export two simple namespaces instead of many globals:
     * host.* for board/runtime helpers, m9p.* for protocol helpers.
     */
    luaL_newlib(L, host_lib);
    lua_setglobal(L, "host");

    luaL_newlib(L, m9p_lib);
    lua_setglobal(L, "m9p");
}
