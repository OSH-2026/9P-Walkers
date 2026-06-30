/*
** $Id: linit.c $
** Initialization of libraries for lua.c and other clients
** See Copyright Notice in lua.h
*/


#define linit_c
#define LUA_LIB


#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "lualib.h"
#include "lauxlib.h"
#include "llimits.h"


typedef struct luaL_EmbedLib {
  const char *name;
  lua_CFunction func;
  int mask;
} luaL_EmbedLib;


/*
** Standard Libraries. (Must be listed in the same ORDER of their
** respective constants LUA_<libname>K.)
*/
static const luaL_EmbedLib stdlibs[] = {
  {LUA_GNAME, luaopen_base, LUA_GLIBK},
  {LUA_MATHLIBNAME, luaopen_math, LUA_MATHLIBK},
  {LUA_STRLIBNAME, luaopen_string, LUA_STRLIBK},
  {LUA_TABLIBNAME, luaopen_table, LUA_TABLIBK},
  {NULL, NULL, 0}
};


/*
** require and preload selected standard libraries
*/
LUALIB_API void luaL_openselectedlibs (lua_State *L, int load, int preload) {
  const luaL_EmbedLib *lib;
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  for (lib = stdlibs; lib->name != NULL; lib++) {
    if (load & lib->mask) {  /* selected? */
      luaL_requiref(L, lib->name, lib->func, 1);  /* require library */
      lua_pop(L, 1);  /* remove result from the stack */
    }
    else if (preload & lib->mask) {  /* selected? */
      lua_pushcfunction(L, lib->func);
      lua_setfield(L, -2, lib->name);  /* add library to PRELOAD table */
    }
  }
  lua_pop(L, 1);  /* remove PRELOAD table */
}

