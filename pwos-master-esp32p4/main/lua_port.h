#ifndef PWOS_MASTER_LUA_PORT_H
#define PWOS_MASTER_LUA_PORT_H

#include <stdbool.h>

struct lua_State;

// 创建并初始化 Lua VM
bool pw_lua_init(void);
// 获取全局 Lua VM 实例
struct lua_State *pw_lua_state(void);
// 执行已经存在于内存中的 Lua 代码块
int pw_lua_run_buffer(const char *chunk_name, const char *source);
// 运行内置的 demo 脚本
int pw_lua_run_demo(void);

#endif
