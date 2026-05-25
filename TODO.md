# TODO

## Mini9P 多 UART 与共享 backend 串行化

- 当前 `mini9p_service` 默认安全前提是单 UART、单 `m9p_server`、单线程轮询访问 backend。
- 如果未来多个 UART/service 同时访问同一个 `node_vfs/lfs_vfs/dev_vfs`，需要引入 backend 串行化机制。
- 推荐方向：每条 UART 保持独立 `m9p_server/mesh_node_runtime/transport`，共享 backend 通过单 backend worker 队列串行处理。
- `mini9p_service_backend` 当前可注入 worker proxy ops，但它本身不提供队列、锁、超时或取消语义。
- 后续可把 `uart` 从 `mini9p_service_backend` 拆到 `mini9p_service_config`，让 backend 注入和 transport 配置职责分离。
