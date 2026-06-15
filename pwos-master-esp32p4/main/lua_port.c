#include "lua_port.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "lua_bindings.h"
#include "shell.h"

// 静态 全局 Lua VM 实例
static lua_State *g_lua_state;

/**
 * @brief 重定向 Lua 内存分配到 ESP-IDF 的 heap_caps 分配器
 * 
 * @param ud 上下文指针
 * @param ptr 当前内存块指针
 * @param osize old size，Lua 传递的当前内存块大小
 * @param nsize new size，Lua 需要的新内存块大小
 * @return void* 分配的内存指针
 */
static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;

    // 若 nsize == 0，释放内存
    if (nsize == 0u) {
        heap_caps_free(ptr);
        return NULL;
    }

    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);
}

/**
 * @brief Lua panic 处理函数，打印错误信息并返回
 * 
 * @param L Lua 状态机指针
 * @return int 返回值，通常为 0
 */
static int lua_panic_handler(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    char buf[160];

    snprintf(buf, sizeof(buf), "lua panic: %s\n", message != NULL ? message : "(unknown)");
    shell_write(buf);
    return 0;
}

static int pw_lua_print(lua_State *L)
{
    int n = lua_gettop(L);

    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char *text = luaL_tolstring(L, i, &len);

        if (i > 1) {
            shell_write("\t");
        }
        if (text != NULL) {
            char buf[160];
            while (len > 0u) {
                size_t chunk = len < sizeof(buf) - 1u ? len : sizeof(buf) - 1u;
                memcpy(buf, text, chunk);
                buf[chunk] = '\0';
                shell_write(buf);
                text += chunk;
                len -= chunk;
            }
        }
        lua_pop(L, 1);
    }
    shell_write("\n");
    return 0;
}

/**
 * @brief 打开最小化的 Lua 标准库
 * 
 * @param L Lua 状态机指针
 */
static void open_minimal_libs(lua_State *L)
{
    struct lua_lib_entry {
        const char *name;
        lua_CFunction open_func;
    };
    static const struct lua_lib_entry libs[] = {
        {"_G", luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    size_t i;

    for (i = 0u; i < sizeof(libs) / sizeof(libs[0]); ++i) {
        luaL_requiref(L, libs[i].name, libs[i].open_func, 1);
        lua_pop(L, 1);  // 弹出栈顶的库表
    }

    lua_pushcfunction(L, pw_lua_print);
    lua_setglobal(L, "print");
}

/**
 * @brief 打印 Lua 脚本执行结果
 * 
 * @param L Lua 状态机指针
 * @param result_count 返回值数量
 */
static void print_results(lua_State *L, int result_count)
{
    int first_index;
    int i;

    if (result_count <= 0) {
        return;
    }

    first_index = lua_gettop(L) - result_count + 1;
    shell_write("lua result:\n");
    for (i = 0; i < result_count; ++i) {
        const char *text = luaL_tolstring(L, first_index + i, NULL);
        char buf[160];

        snprintf(buf, sizeof(buf), "  %s\n", text != NULL ? text : "(nil)");
        shell_write(buf);
        lua_pop(L, 1);  // 弹出 luaL_tolstring 生成的字符串
    }
}

/**
 * @brief Lua 环境初始化函数，创建 Lua VM 并加载必要的库和绑定
 * 
 * @return true 初始化成功
 * @return false 初始化失败
 */
bool pw_lua_init(void)
{
    if (g_lua_state != NULL) {
        return true;
    }

    g_lua_state = lua_newstate(lua_alloc, NULL, 0u);
    if (g_lua_state == NULL) {
        shell_write("lua init failed: allocator returned NULL\n");
        return false;
    }

    lua_atpanic(g_lua_state, lua_panic_handler);
    open_minimal_libs(g_lua_state);
    // 注册 pwos 绑定到 Lua 环境
    pw_lua_register_bindings(g_lua_state);
    return true;
}

/**
 * @brief 获取全局 Lua VM 实例指针
 * 
 * @return lua_State* 
 */
lua_State *pw_lua_state(void)
{
    return g_lua_state;
}

/**
 * @brief 执行 Lua 脚本缓冲区
 * 
 * @param chunk_name 脚本名称
 * @param source 脚本内容
 * @return int 返回状态码
 */
int pw_lua_run_buffer(const char *chunk_name, const char *source)
{
    int base;
    int status;
    int result_count;
    char load_error[192];

    if (source == NULL) {
        return -1;
    }
    if (g_lua_state == NULL && !pw_lua_init()) {
        return -1;
    }

    // 记住旧的栈顶 base
    base = lua_gettop(g_lua_state);
    status = luaL_loadbuffer(g_lua_state, source, strlen(source), chunk_name != NULL ? chunk_name : "chunk");
    if (status != LUA_OK) {
        const char *message = lua_tostring(g_lua_state, -1);
        char expr_source[320];
        int written;

        snprintf(load_error, sizeof(load_error), "lua load error: %s\n", message != NULL ? message : "(unknown)");
        lua_settop(g_lua_state, base);  // 恢复旧的栈顶

        /*
         * Shell 上的一行 Lua 常被当作 REPL 表达式输入，例如：
         *   lua 1 + 1
         * Lua chunk 语法不允许裸表达式，因此失败后再按 "return <expr>" 尝试一次。
         */
        written = snprintf(expr_source, sizeof(expr_source), "return %s", source);
        if (written > 0 && (size_t)written < sizeof(expr_source)) {
            int expr_status = luaL_loadbuffer(
                g_lua_state,
                expr_source,
                strlen(expr_source),
                chunk_name != NULL ? chunk_name : "chunk");
            if (expr_status == LUA_OK) {
                status = LUA_OK;
            } else {
                shell_write(load_error);
                lua_settop(g_lua_state, base);
                return status;
            }
        } else {
            shell_write(load_error);
            return status;
        }
    }

    // 用 g_lua_state 启动脚本
    // 0: 无参数，LUA_MULTRET: 返回所有结果，0: 无错误函数
    status = lua_pcall(g_lua_state, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        char buf[192];

        snprintf(buf, sizeof(buf), "lua runtime error: %s\n", lua_tostring(g_lua_state, -1));
        shell_write(buf);
        lua_settop(g_lua_state, base);  // 恢复旧的栈顶
        return status;
    }

    // 到这里说明脚本成功执行，打印返回值
    result_count = lua_gettop(g_lua_state) - base;
    print_results(g_lua_state, result_count);
    lua_settop(g_lua_state, base);  // 恢复旧的栈顶
    return 0;
}

int pw_lua_run_demo(void)
{
    static const char demo_script[] =
        "print('Lua runtime ready on ESP32')\n"
        "print('free heap', host.free_heap())\n"
        "print('uptime ms', host.uptime_ms())\n"
        "local chip = host.chip()\n"
        "print('chip', chip.target, chip.cores, chip.revision)\n"
        "print('mini9p version', m9p.version())\n"
        "print('m9p ENOENT', m9p.error_name(2))\n"
        "host.echo('bindings are live')\n"
        /* Exercise the vfs.* bindings. With no routes attached yet the root
         * listing is simply empty, so this demonstrates the binding works
         * without requiring a slave to be connected. */
        "local items, err = vfs.list('/')\n"
        "if items then\n"
        "  print('vfs.list / ->', #items, 'node(s)')\n"
        "  for i, e in ipairs(items) do\n"
        "    print('  '..i..': '..e.name..(e.is_dir and '/' or ''))\n"
        "  end\n"
        "else\n"
        "  print('vfs.list / failed:', err)\n"
        "end\n"
        "return 6 * 7\n";

    return pw_lua_run_buffer("boot_demo", demo_script);
}
