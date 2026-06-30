#ifndef PWOS_LUA_RENDER_RUNTIME_H
#define PWOS_LUA_RENDER_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* 启动独立 Lua VM，并在后台持续执行 Whitted tile 调度脚本。 */
int pwos_lua_render_runtime_start(void);

#ifdef __cplusplus
}
#endif

#endif /* PWOS_LUA_RENDER_RUNTIME_H */
