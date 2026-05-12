# Backend

`User/backend` 放置 Mini9P 从机侧的本地后端实现。当前版本只有一个极简 `local_vfs`，用于先跑通从 PC 到 STM32 的 Mini9P 闭环。

## 当前实现

`local_vfs` 是一个只读虚拟 VFS，通过 `m9p_server_ops` 接入 `mini9p_server`。它不挂载 littlefs，也不保存文件句柄；fid、会话状态和权限检查仍由 `mini9p_server` 负责。

当前暴露的虚拟树：

```text
/
└── sys
    └── health
```

`/sys/health` 是只读虚拟文件，内容为：

```text
ok
```

实际字节内容为 `ok\n`，大小为 3。

## 文件说明

- `local_vfs.h`：对外接口，只暴露初始化和 `m9p_server_ops` 获取函数。
- `local_vfs.c`：虚拟节点表、stat/open/read/write/clunk 实现。
- `test/`：local VFS 的 PC 侧最小单元测试。

## 使用方式

典型接入方式：

```c
struct local_vfs vfs;
struct local_vfs_config vfs_config;
struct m9p_server_config server_config;

local_vfs_get_default_config(&vfs_config);
local_vfs_init(&vfs, &vfs_config);

m9p_server_get_default_config(&server_config);
server_config.ops = local_vfs_ops();
server_config.ops_ctx = &vfs;
m9p_server_init(&server, &server_config);
```

## 测试

```bash
cmake -S pwos-slave/User/backend/test -B pwos-slave/User/backend/test/build
cmake --build pwos-slave/User/backend/test/build
pwos-slave/User/backend/test/build/local_vfs_test
```

测试覆盖：

- `stat("/")`
- `stat("/sys")`
- `stat("/sys/health")`
- `open/read /sys/health`
- 根目录和 `/sys` 的目录项读取
- 不存在路径返回 `ENOENT`
- 写入返回 `ENOTSUP`

## 当前边界

- 暂不接入 littlefs。
- 暂无动态注册节点。
- 暂无设备节点。
- 目录项编码仍在 `local_vfs.c` 内完成。
- 仍保持 path-based `m9p_server_ops`，暂未改成 handle-based。

后续 littlefs 可以作为 provider 接入，但不应影响当前虚拟节点 smoke test。
