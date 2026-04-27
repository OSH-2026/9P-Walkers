#ifndef PWOS_MASTER_LUA_BINDINGS_H
#define PWOS_MASTER_LUA_BINDINGS_H

struct lua_State;

void pw_lua_register_bindings(struct lua_State *L);

#endif
