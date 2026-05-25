# Backend

`User/backend` 放置 Mini9P 从机侧的本地后端实现。当前 pwos-slave 运行时默认使用 `lfs_vfs`，把 littlefs 文件树直接通过 `m9p_server_ops` 暴露给 Mini9P；`local_vfs` 仍保留在仓库中，作为早期只读虚拟树和本地单测样例。

## 当前实现

`lfs_vfs` 是当前主用 backend。它通过 `m9p_server_ops` 接入 `mini9p_server`，在 `stat/open/read/write/clunk` 五个回调里把 Mini9P path-based 请求直接映射到 littlefs。

上电后默认会由 `fs_selftest` 预置一组便于联调的 littlefs 内容：

```text
/
└── verify
    ├── fs_selftest.txt
    ├── debug
    │   ├── backend.txt
    │   ├── report.txt
    │   └── run_count.txt
    └── tree
        ├── nested
        │   └── config.txt
        └── readme.txt
```

## 文件说明

- `lfs_vfs.h`：littlefs backend 对外接口。
- `lfs_vfs.c`：littlefs 到 Mini9P 的路径映射、目录流读取和读写回调实现。
- `local_vfs.h/.c`：旧的只读虚拟树 backend，当前不参与 pwos-slave 运行时构建。
- `test/`：legacy `local_vfs` 的 PC 侧最小单元测试。

## 使用方式

典型接入方式：

```c
struct lfs_vfs vfs;
struct lfs_vfs_config vfs_config;
struct m9p_server_config server_config;

lfs_vfs_get_default_config(&vfs_config);
lfs_vfs_init(&vfs, &vfs_config);

m9p_server_get_default_config(&server_config);
server_config.ops = lfs_vfs_ops();
server_config.ops_ctx = &vfs;
m9p_server_init(&server, &server_config);
```

## 测试

```bash
cmake -S pwos-slave/User/backend/test -B pwos-slave/User/backend/test/build
cmake --build pwos-slave/User/backend/test/build
pwos-slave/User/backend/test/build/local_vfs_test
```

legacy `local_vfs` 测试覆盖：

- `stat("/")`
- `stat("/sys")`
- `stat("/sys/health")`
- `open/read /sys/health`
- 根目录和 `/sys` 的目录项读取
- 不存在路径返回 `ENOENT`
- 写入返回 `ENOTSUP`

## 当前边界

- Mini9P 仍保持 path-based `m9p_server_ops`，没有 create/remove 等更丰富的文件系统语义。
- 当前 pwos-slave 默认依赖 SDIO + littlefs，因此串口 Mini9P 模式下也需要先初始化 SDIO。
