# vfs_bridge TODO

本文记录 `cluster_vfs` 后续待改进点。这里偏工程待办；长期架构解释仍放在 `design.md`。

## 近期优先

- [ ] 明确 `cluster_vfs_add_route()` 的处理方式。
  - 当前头文件已声明，但 `cluster_vfs.c` 尚未实现。
  - 近期可选方案：先移除公开声明，或实现返回 `-M9P_ERR_ENOTSUP` 的占位函数。
  - 长期目标是支持 `target != next_hop` 的静态一跳中继路由。

- [ ] 决定 `attach()` / `detach()` 是否做成幂等接口。
  - 当前 `attach()` 只接受 `READY -> ATTACHED`。
  - 当前 `detach()` 只接受 `ATTACHED -> READY`。
  - 可考虑：已处于目标状态时直接返回 `0`，不存在 target 才返回 `ENOENT`。

## 中期功能

- [ ] 定义并使用 `CLUSTER_VFS_ROUTE_OFFLINE`。
  - 当前枚举已预留，但实现未主动设置该状态。
  - 可考虑 attach 失败或通信失败后标记为 `OFFLINE`。
  - 需要同时定义从 `OFFLINE` 重试 attach 的规则。

- [ ] 增加批量路由枚举接口。
  - 目标接口形态：
    ```c
    int cluster_vfs_list_routes(struct cluster_vfs_route_info *routes,
                                size_t max_routes,
                                size_t *out_count);
    ```
  - 用途：Web 拓扑、Shell `nodes` 命令、调试输出。
  - 注意只导出 `target/next_hop/state` 等副本，不暴露内部路由指针。

- [ ] 增加路径规范化策略。
  - 当前主要接受 `/mcuN/...` 形式。
  - 后续需明确 `//`、`.`、`..`、尾部多余 `/` 的处理方式。
  - 建议先拒绝复杂路径，避免路径绕过或映射歧义。

## 集成事项

- [ ] Shell 普通文件命令接入 VFS。
  - `ls` 应调用 `cluster_vfs_list()`。
  - `cat` 应调用 `cluster_vfs_read_path()`。
  - `echo > path` 应调用 `cluster_vfs_write_path()`。
  - `m9p_*` 命令可保留为协议调试入口。

- [ ] Lua/Web 统一通过 VFS 访问节点资源。
  - Lua 的 `read/write/list/stat` 绑定应落到 `cluster_vfs_*`。
  - Web API 或 WebShell 不应直接操作 `mini9p_client`。

- [ ] 考虑把 `cluster_config` 移到独立 `cluster/` 层。
  - 当前 `cluster_config.c` 临时位于 `vfs_bridge/`。
  - 它负责静态节点注册，更接近“节点管理/配置”职责。
  - 长期可迁移到 `pwos-master-esp32p4/cluster/`，由该层调用 `cluster_vfs_add_direct()`。

## 暂不做

- [ ] 多跳 routing header。
- [ ] 动态路由发现。
- [ ] 自动重连和目录缓存。
- [ ] 并发锁和跨任务 fd 访问保护。
