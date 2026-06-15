# vfs_bridge TODO

本文记录 `cluster_host_vfs` 后续待改进点。

## 近期优先

- [ ] 决定 `attach()` / `detach()` 是否做成幂等接口。
  - 当前 `attach()` 只接受 `NEW -> ATTACHED`。
  - 当前 `detach()` 只接受 `ATTACHED -> NEW`。
  - 可考虑：已处于目标状态时直接返回 `0`，不存在 target 才返回 `ENOENT`。

## 中期功能

- [ ] 完善离线后的恢复策略。
  - 当前在线快照由 mesh cluster 连通性和 VFS 当前 mesh 地址绑定派生。
  - 需要定义不可达后重新 discover、重新 attach 的规则。

- [ ] 增加批量路由枚举接口。
  - 目标接口形态：
    ```c
    int cluster_vfs_list_routes(struct cluster_vfs_route_info *routes,
                                size_t max_routes,
                                size_t *out_count);
    ```
  - 用途：Web 拓扑、Shell `nodes` 命令、调试输出。
  - 注意只导出 `target/mesh_addr` 等必要副本，不暴露内部节点映射指针。

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

## 已完成（从旧 TODO 迁移）

- [x] 多跳 routing header（mesh envelope 已支持 hop/TTL 和 route_lookup）。
- [x] 动态路由发现（主机维护拓扑并派生路由，向从机下发 ROUTE_UPDATE）。
- [x] `cluster_config` 已迁移到 `pwos-master-esp32p4/cluster/`。

## 暂不做

- [ ] 自动重连和目录缓存。
- [ ] 并发锁和跨任务 fd 访问保护。
