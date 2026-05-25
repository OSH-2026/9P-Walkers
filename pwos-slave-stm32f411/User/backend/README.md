# Backend

`User/backend` 放置 Mini9P 从机侧的本地后端实现。架构分为两层：

- **叶后端**：`lfs_vfs`、`dev_vfs`、`sys_vfs` 各自实现一棵虚拟文件树。
- **路由层**：`node_vfs` 是统一入口，按路径前缀将请求分发到对应叶后端。

```
/ (node_vfs 根目录，列出 sys、dev、fs)
├── /sys/*  -> sys_vfs  (sys、health、info)
├── /dev/*  -> dev_vfs  (dev、led)
└── /fs/*  -> lfs_vfs   (littlefs 根，/fs 前缀被剥离)
```

`local_vfs` 是旧的只读虚拟树实现，当前不参与运行时构建，仅保留作参考。

## 各模块说明

| 文件 | 职责 | 虚拟节点 |
|------|------|----------|
| `lfs_vfs.h/.c` | littlefs 文件系统后端 | `/` 下的littlefs文件树 |
| `dev_vfs.h/.c` | 设备虚拟文件系统 | `/dev`、`/dev/led` |
| `sys_vfs.h/.c` | 系统信息虚拟文件系统 | `/sys`、`/sys/health`、`/sys/info` |
| `node_vfs.h/.c` | 多后端路由分发器 | 根目录列出 sys/dev/fs |
| `local_vfs.h/.c` | 旧只读虚拟树，不参与构建 | - |

## 典型接入方式

```c
struct lfs_vfs lfs;
struct dev_vfs dev;
struct sys_vfs sys;
struct node_vfs node;
struct node_vfs_config node_cfg = {
    .sys_ops  = sys_vfs_ops(),
    .sys_ctx  = &sys,
    .dev_ops  = dev_vfs_ops(),
    .dev_ctx  = &dev,
    .lfs_ops  = lfs_vfs_ops(),
    .lfs_ctx  = &lfs,
};
node_vfs_init(&node, &node_cfg);

// 将 node_ops() 接入 mini9p_server
```

三个后端统一使用 ops/ctx 模式，配置结构一致。

## dev_vfs 设备操作接口

`dev_vfs` 通过 `struct dev_vfs_device_ops` 回调接入真实硬件：

```c
struct dev_vfs_device_ops {
    int (*read_led)(void *ctx, char *out, uint16_t out_cap, uint16_t *out_len);
    int (*write_led)(void *ctx, const uint8_t *data, uint16_t len, uint16_t *out_written);
};
```

## sys_vfs info_text

`sys_vfs` 的 `info_text` 字段可传入自定义文本，作为 `/sys/info` 的内容。

## 当前边界

- Mini9P 保持 path-based `m9p_server_ops`，无 create/remove 等语义。
- 当前默认依赖 SDIO + littlefs，串口 Mini9P 模式需先初始化 SDIO。
- `/sys/health` 只读，`/sys/info` 由 `info_text` 配置。
- `/dev/led` 支持读写，回调由 `device_ops` 提供。