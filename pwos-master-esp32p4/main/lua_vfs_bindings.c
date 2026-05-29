#include "lua_vfs_bindings.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cluster_host_vfs.h"
#include "lauxlib.h"
#include "lua.h"
#include "mini9p_client.h"
#include "mini9p_protocol.h"

/* 当调用者没有提供长度时，流式读取的默认分块大小。 */
#define VFS_READ_CHUNK   256u
/* vfs.list 在一次调用中返回的目录项数的上限。 */
#define VFS_LIST_MAX     32u

/*
 * cluster_vfs_* 成功时返回 0，失败时返回负的 Mini9P 错误码
 * （即 -(int)M9P_ERR_xxx）。将负的 rc 转换为 Lua 习惯用法
 * (nil, "<ERRNAME>")，并返回压入栈的值的数量 (2)。
 */
static int push_vfs_error(lua_State *L, int rc)
{
    uint16_t code = (rc < 0) ? (uint16_t)(-rc) : 0u;
    lua_pushnil(L);
    lua_pushstring(L, m9p_error_name(code));
    return 2;
}

/* 验证 Lua 整数是否适合 cluster_vfs 使用的 uint16_t fd 空间。 */
static uint16_t check_fd(lua_State *L, int arg)
{
    lua_Integer fd = luaL_checkinteger(L, arg);
    if (fd < 0 || fd > UINT16_MAX) {
        luaL_error(L, "fd out of range: %lld", (long long)fd);
    }
    return (uint16_t)fd;
}

/* ------------------------------------------------------------------ */
/* 高级路径 API                                                         */
/* ------------------------------------------------------------------ */

/* vfs.read(path) -> string | nil, errname
 * 通过不断读取 VFS_READ_CHUNK 字节来流式读取整个文件。 */
static int l_vfs_read(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    uint16_t fd;
    int rc = cluster_vfs_open(path, M9P_OREAD, &fd);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }

    luaL_Buffer buf;
    luaL_buffinit(L, &buf);

    for (;;) {
        uint8_t chunk[VFS_READ_CHUNK];
        uint16_t len = (uint16_t)sizeof(chunk);
        rc = cluster_vfs_read(fd, chunk, &len);
        if (rc < 0) {
            (void)cluster_vfs_close(fd);
            return push_vfs_error(L, rc);
        }
        if (len == 0u) {
            break; /* EOF (文件结束) */
        }
        luaL_addlstring(&buf, (const char *)chunk, len);
    }

    int close_rc = cluster_vfs_close(fd);
    if (close_rc < 0) {
        return push_vfs_error(L, close_rc);
    }

    luaL_pushresult(&buf);
    return 1;
}

/* vfs.write(path, data) -> bytes_written | nil, errname */
static int l_vfs_write(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t data_len;
    const char *data = luaL_checklstring(L, 2, &data_len);

    if (data_len > UINT16_MAX) {
        return luaL_error(L, "data too large: %u bytes (max %u)",
                          (unsigned)data_len, (unsigned)UINT16_MAX);
    }

    uint16_t written = 0u;
    int rc = cluster_vfs_write_path(path, (const uint8_t *)data,
                                    (uint16_t)data_len, &written);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }

    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

/* vfs.list(path) -> { {name=,is_dir=,qid_type=}, ... } | nil, errname */
static int l_vfs_list(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    struct m9p_dirent *entries = calloc(VFS_LIST_MAX, sizeof(*entries));
    if (entries == NULL) {
        return luaL_error(L, "out of memory allocating dirent buffer");
    }

    size_t count = 0u;
    int rc = cluster_vfs_list(path, entries, VFS_LIST_MAX, &count);
    if (rc < 0) {
        free(entries);
        return push_vfs_error(L, rc);
    }

    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; ++i) {
        lua_createtable(L, 0, 3);

        lua_pushstring(L, entries[i].name);
        lua_setfield(L, -2, "name");

        lua_pushboolean(L, (entries[i].qid.type & M9P_QID_DIR) != 0u);
        lua_setfield(L, -2, "is_dir");

        lua_pushinteger(L, (lua_Integer)entries[i].qid.type);
        lua_setfield(L, -2, "qid_type");

        lua_rawseti(L, -2, (lua_Integer)(i + 1u)); /* Lua 数组索引从 1 开始 */
    }

    free(entries);
    return 1;
}

/* vfs.stat(path) -> {name,size,mtime,perm,is_dir} | nil, errname */
static int l_vfs_stat(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    struct m9p_stat st;
    int rc = cluster_vfs_stat(path, &st);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }

    lua_createtable(L, 0, 5);
    lua_pushstring(L, st.name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)st.size);
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, (lua_Integer)st.mtime);
    lua_setfield(L, -2, "mtime");
    lua_pushinteger(L, (lua_Integer)st.perm);
    lua_setfield(L, -2, "perm");
    lua_pushboolean(L, (st.qid.type & M9P_QID_DIR) != 0u);
    lua_setfield(L, -2, "is_dir");

    return 1;
}

/* ------------------------------------------------------------------ */
/* 低级 fd API                                                          */
/* ------------------------------------------------------------------ */

/* vfs.open(path[, mode]) -> fd | nil, errname  (mode 默认为 OREAD) */
static int l_vfs_open(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_Integer mode = luaL_optinteger(L, 2, M9P_OREAD);

    uint16_t fd;
    int rc = cluster_vfs_open(path, (uint8_t)mode, &fd);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }

    lua_pushinteger(L, (lua_Integer)fd);
    return 1;
}

/* vfs.read_fd(fd[, len]) -> string | nil, errname  (len 默认为 chunk) */
static int l_vfs_read_fd(lua_State *L)
{
    uint16_t fd = check_fd(L, 1);
    lua_Integer want = luaL_optinteger(L, 2, (lua_Integer)VFS_READ_CHUNK);
    if (want <= 0 || want > UINT16_MAX) {
        return luaL_error(L, "length out of range: %lld", (long long)want);
    }

    uint8_t *buf = malloc((size_t)want);
    if (buf == NULL) {
        return luaL_error(L, "out of memory allocating read buffer");
    }

    uint16_t len = (uint16_t)want;
    int rc = cluster_vfs_read(fd, buf, &len);
    if (rc < 0) {
        free(buf);
        return push_vfs_error(L, rc);
    }

    lua_pushlstring(L, (const char *)buf, len);
    free(buf);
    return 1;
}

/* vfs.write_fd(fd, data) -> bytes_written | nil, errname */
static int l_vfs_write_fd(lua_State *L)
{
    uint16_t fd = check_fd(L, 1);
    size_t data_len;
    const char *data = luaL_checklstring(L, 2, &data_len);

    if (data_len > UINT16_MAX) {
        return luaL_error(L, "data too large: %u bytes", (unsigned)data_len);
    }

    uint16_t written = 0u;
    int rc = cluster_vfs_write(fd, (const uint8_t *)data,
                               (uint16_t)data_len, &written);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }

    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

/* vfs.close(fd) -> true | nil, errname */
static int l_vfs_close(lua_State *L)
{
    uint16_t fd = check_fd(L, 1);
    int rc = cluster_vfs_close(fd);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 路由管理 API                                                         */
/* ------------------------------------------------------------------ */

/* vfs.attach(target) -> true | nil, errname */
static int l_vfs_attach(lua_State *L)
{
    const char *target = luaL_checkstring(L, 1);
    int rc = cluster_vfs_attach(target);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* vfs.detach(target) -> true | nil, errname */
static int l_vfs_detach(lua_State *L)
{
    const char *target = luaL_checkstring(L, 1);
    int rc = cluster_vfs_detach(target);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* vfs.route_state(target) -> "empty"|"ready"|"attached"|"offline" | nil,err */
static int l_vfs_route_state(lua_State *L)
{
    const char *target = luaL_checkstring(L, 1);
    enum cluster_vfs_route_state state;
    int rc = cluster_vfs_get_route_state(target, &state);
    if (rc < 0) {
        return push_vfs_error(L, rc);
    }

    const char *name;
    switch (state) {
    case CLUSTER_VFS_ROUTE_EMPTY:    name = "empty";    break;
    case CLUSTER_VFS_ROUTE_READY:    name = "ready";    break;
    case CLUSTER_VFS_ROUTE_ATTACHED: name = "attached"; break;
    case CLUSTER_VFS_ROUTE_OFFLINE:  name = "offline";  break;
    default:                         name = "unknown";  break;
    }
    lua_pushstring(L, name);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 注册                                                                 */
/* ------------------------------------------------------------------ */

void pw_lua_register_vfs_bindings(lua_State *L)
{
    static const luaL_Reg vfs_lib[] = {
        /* 高级路径助手函数 */
        {"read",        l_vfs_read},
        {"write",       l_vfs_write},
        {"list",        l_vfs_list},
        {"stat",        l_vfs_stat},
        /* 低级 fd 生命周期 */
        {"open",        l_vfs_open},
        {"read_fd",     l_vfs_read_fd},
        {"write_fd",    l_vfs_write_fd},
        {"close",       l_vfs_close},
        /* 路由管理 */
        {"attach",      l_vfs_attach},
        {"detach",      l_vfs_detach},
        {"route_state", l_vfs_route_state},
        {NULL, NULL},
    };

    luaL_newlib(L, vfs_lib);

    /* Open 模式常量，以便脚本读取 vfs.OWRITE 而不是魔术整数。 */
    lua_pushinteger(L, M9P_OREAD);
    lua_setfield(L, -2, "OREAD");
    lua_pushinteger(L, M9P_OWRITE);
    lua_setfield(L, -2, "OWRITE");
    lua_pushinteger(L, M9P_ORDWR);
    lua_setfield(L, -2, "ORDWR");

    lua_setglobal(L, "vfs");
}
