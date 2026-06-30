# Backend

`local_vfs` 是当前 Mini9P server 的唯一后端入口。它拥有固定命名空间和目录
分页规则，文件内容由 `service_runtime` 的回调按 offset 流式生成，不分配大缓冲。

```text
/
└── /sys
    ├── health      tasks       ports       links
    ├── neighbors   routes      sessions    queues
    ├── log         build       fault
    └── info/uart   compatibility aliases
```

## 所有权

- `local_vfs`：路径、qid、权限和完整 dirent 分页。
- `service_runtime`：任务、队列、端口、路由、session 等运行时文本。
- `fault_control`：Debug 构建的故障注入状态；`/sys/fault` 是唯一可写节点。
- `mini9p_server`：fid、open mode、offset 和请求响应协议。

旧的 `node_vfs/sys_vfs/dev_vfs/lfs_vfs` 未进入重构后运行时，已经删除。存储
驱动仍独立保留，后续重新接入 `/fs` 时应基于当前 session/并发约束设计新后端。
