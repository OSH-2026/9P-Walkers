#ifndef PWOS_MASTER_LUA_VFS_BINDINGS_H
#define PWOS_MASTER_LUA_VFS_BINDINGS_H

/*
 * pwos-master-esp32p4 的 Lua-VFS 绑定。
 *
 * 注册后，脚本可以通过单个全局 `vfs` 表操作集群 VFS：
 *
 *   高级 API（基于路径，内部处理 open/read/close）：
 *     local data, err = vfs.read(path)             -- 返回 string | nil,err
 *     local n,   err = vfs.write(path, data)        -- 返回 int    | nil,err
 *     local items,err = vfs.list(path)              -- 返回 array  | nil,err
 *     local st,  err = vfs.stat(path)               -- 返回 table  | nil,err
 *
 *   低级 API（显式的本地 fd 生命周期）：
 *     local fd, err  = vfs.open(path, mode)         -- mode: vfs.OREAD / ...
 *     local data,err = vfs.read_fd(fd, n)
 *     local nw, err  = vfs.write_fd(fd, data)
 *     local ok, err  = vfs.close(fd)
 *
 *   路由管理（映射 cluster_vfs API）：
 *     vfs.attach(target), vfs.detach(target),
 *     vfs.route_state(target) -> "empty"|"ready"|"attached"|"offline"
 *
 *   常量：
 *     vfs.OREAD, vfs.OWRITE, vfs.ORDWR
 *
 * 错误处理：
 *   失败时，每次调用返回 (nil, error_name_string)。该字符串是由
 *   m9p_error_name 返回的符号化 Mini9P 错误（例如 "ENOENT"）。
 */

struct lua_State;

void pw_lua_register_vfs_bindings(struct lua_State *L);

#endif
