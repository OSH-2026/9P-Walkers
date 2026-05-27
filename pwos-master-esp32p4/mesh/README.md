# Master mesh runtime

`pwos-master-esp32p4/mesh/` 包含主机侧的粘合层，将共享 mesh 层连接到 ESP32-P4 主机运行时。

## 文件说明

- `mesh_host_runtime.c/.h`
  - 拥有主机 mesh runtime 对象。
  - 封装共享的 `mesh_processer`。
  - 处理 `REGISTER`、`LINK_STATE` 和 `ROUTE_UPDATE` 等 mesh 控制帧。
  - 为已发现的 mesh 节点创建长期存在的 Mini9P 客户端，并将其注册到 `cluster_config` / `cluster_vfs`。
  - 提供默认初始化和 ESP 轮询任务入口点。

- `mesh_transport_manager.c/.h`
  - 拥有一个或多个原始 mesh UART 传输端口。
  - 提供与 `mesh_processer` 兼容的发送/接收回调。
  - 在多端口配置下，根据 `next_hop` 选择出口 UART。
  - 将所有已配置 UART 的入口帧多路复用为 `mesh_host_runtime` 期望的单一接收流。

## 运行时流程

```text
cluster_vfs / Mini9P client
  -> mesh_host_runtime_client_request()
  -> mesh envelope MINI9P 帧
  -> mesh_transport_manager_send_frame(next_hop, ...)
  -> mesh_uart_transport_send_frame(选中的 UART)
```

入站帧沿反向路径：

```text
mesh_uart_transport_receive_frame(任意已配置的 UART)
  -> mesh_transport_manager_receive_frame()
  -> mesh_processer_poll_once() / mesh_host_runtime_process_frame()
  -> 控制处理器或 Mini9P 响应分发
```

## 传输管理器模型

传输管理器有意设计为静态且嵌入式友好：

- 无动态分配；
- 通过 `MESH_TRANSPORT_MANAGER_MAX_PORTS` 限制最大端口数；
- 每个已配置端口一个 `struct mesh_uart_transport` 实例；
- 静态 `neighbor_addr -> UART 端口` 映射。

`neighbor_addr` 是该 UART 上直接相连的下一跳 mesh 地址，而非最终目的地址。例如：

```text
host -- UART1 -- mcu1(0x11) -- mcu2(0x22)
host -- UART2 -- mcu3(0x33)
```

发往 `mcu2` 的请求通过集群路由解析得到 `next_hop = 0x11`；管理器随后在配置了 `neighbor_addr = 0x11` 的 UART 上发送。

单端口默认模式使用 `MESH_TRANSPORT_MANAGER_NEIGHBOR_ANY`，因此现有默认运行时初始化继续通过唯一配置的 UART 发送所有流量。

## 当前限制

- `mesh_host_runtime` 仍具有全局单事务语义（通过 `dispatch_busy`）。
- 接收回调不向运行时暴露入口端口标识。
- 端口路由静态配置；管理器不从接收帧中学习路由。
- 管理器不实现每端口响应队列或并发请求分发。

这些限制保持了当前实现与现有 mesh processor API 的兼容性，同时支持了首个有用的多 UART 主机拓扑。