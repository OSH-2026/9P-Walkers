#ifndef PWOS_MASTER_LUA_VFS_BINDINGS_H
#define PWOS_MASTER_LUA_VFS_BINDINGS_H

/*
 * Lua-VFS bindings for pwos-master-esp32p4.
 *
 * After registration, scripts can manipulate the cluster VFS through a
 * single global `vfs` table:
 *
 *   High-level (path-based, open/read/close handled internally):
 *     local data, err = vfs.read(path)             -- returns string | nil,err
 *     local n,   err = vfs.write(path, data)        -- returns int    | nil,err
 *     local items,err = vfs.list(path)              -- returns array  | nil,err
 *     local st,  err = vfs.stat(path)               -- returns table  | nil,err
 *
 *   Low-level (explicit local fd lifecycle):
 *     local fd, err  = vfs.open(path, mode)         -- mode: vfs.OREAD / ...
 *     local data,err = vfs.read_fd(fd, n)
 *     local nw, err  = vfs.write_fd(fd, data)
 *     local ok, err  = vfs.close(fd)
 *
 *   Route management (mirrors cluster_vfs APIs):
 *     vfs.attach(target), vfs.detach(target),
 *     vfs.route_state(target) -> "empty"|"ready"|"attached"|"offline"
 *
 *   Constants:
 *     vfs.OREAD, vfs.OWRITE, vfs.ORDWR
 *
 * Errors:
 *   On failure each call returns (nil, error_name_string). The string is
 *   the symbolic Mini9P error (e.g. "ENOENT") returned by m9p_error_name.
 */

struct lua_State;

void pw_lua_register_vfs_bindings(struct lua_State *L);

#endif
