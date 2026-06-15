# 从机侧 Mesh Runtime 开发者指南

## 1. 模块定位

从机侧 mesh 运行时分两层：

- `mesh_node_runtime`（`pwos-slave/User/mesh/mesh_node_runtime.c/.h`）：状态机层，负责 bootstrap、地址分配、邻居发现、直连路由、Mini9P 分流。
- `mesh_node_service`（`pwos-slave/User/mesh/mesh_node_service.c/.h`）：板级装配层，管理多个 `mesh_uart_transport` 端口，维护动态的 `addr -> UART port` 映射。

`runtime` 不直接拥有 UART；它通过 `send_frame / send_frame_to_port / receive_frame` 回调与 `service` 交互。

## 2. 初始化顺序

推荐板级入口 `mesh_node_mini9p_init()` 的调用顺序：

1. 初始化 `node_vfs` 与 `mini9p_server`。
2. 填充 `struct mesh_node_service_config`：
   - 设置每个端口的 `uart_config.uart`（STM32 HAL UART handle）。
   - 设置 `mini9p_server_handler` 与 `mini9p_server_ctx`。
3. 调用 `mesh_node_service_init(&config)`：
   - 初始化所有启用端口的 `mesh_uart_transport`。
   - 生成 8 字节 UID（`HAL_GetUIDw0/1/2` 压缩）。
   - 生成 `boot_nonce`。
   - 调用 `mesh_node_runtime_init()`。
4. 主循环中周期性调用 `mesh_node_service_poll_once()`。

## 3. Bootstrap：REGISTER 广播与 ASSIGN 落地

### 3.1 上电注册

`mesh_node_runtime_init()`（`mesh_node_runtime.c:812`）成功后会立即发送一帧 `REGISTER`：

- `src = MESH_ADDR_UNASSIGNED`（`0xFF`）
- `dst = MESH_ADDR_UNASSIGNED`
- payload 包含 `uid`、`boot_nonce`、`capability_bits`、`port_bitmap`

`service` 的 `send_frame` 在 `next_hop == MESH_NODE_SERVICE_NEIGHBOR_ANY`（`0xFF`）时向**所有已启用 UART 端口**广播该帧（`mesh_node_service.c:211`）。

### 3.2 未分配期间重发

在拿到正式地址前，`mesh_node_runtime_poll_once()`（`mesh_node_runtime.c:1080`）会在空闲时按退避间隔重发 `REGISTER`：

- 起始 250 ms，每次翻倍，封顶 1000 ms。
- 不设次数上限；同一 UID 在主机侧幂等，反复重发不会导致地址漂移。

### 3.3 ASSIGN 命中本机

当收到 `ASSIGN` 且 UID 匹配本机时：

1. `mesh_node_runtime_control_handler()`（`mesh_node_runtime.c:428`）更新 `processor.config.local_addr`。
2. 记录 `upstream_port = last_ingress_port`（收到 ASSIGN 的物理口）。
3. 记录 `control_plane_addr = frame->src`（通常是主机地址 `0x00`）。
4. 向所有端口发送 `NEIGHBOR_PROBE_REQUEST`。

## 4. NEIGHBOR_PROBE_REQUEST / RESPONSE 流程

### 4.1 探测触发

以下场景会触发探测：

- 本机 `ASSIGN` 完成后（见 3.3）。
- 收到 `NEIGHBOR_PROBE_REQUEST` 后（回响应前）。
- 主循环空闲时，若 `neighbor_probe_retries_left > 0` 则按 200 ms 间隔重试。

### 4.2 请求与响应语义

- `NEIGHBOR_PROBE_REQUEST`：单链路本地请求，**不进入普通转发路径**。
- 接收端若已有正式地址，立即从同一 `ingress_port` 回 `NEIGHBOR_PROBE_RESPONSE(src=本机地址)`。
- 接收端若为未分配地址（`0xFF`），不回复，靠后续重试收敛。

### 4.3 学习直连邻居

收到 `NEIGHBOR_PROBE_RESPONSE` 后（`mesh_node_runtime.c:981`）：

1. 在 `cluster` 中写入 direct route：`dst=src, next_hop=src, metric=1`。
2. 通过 `learn_peer_port` 回调通知 service 记录 `mesh_addr -> ingress_port`。
3. 若响应来自 `upstream_port`，更新 `upstream_peer_addr`。
4. 若已知 `control_plane_addr`，向 `upstream_port` 上报 `LINK_STATE(src=本机, neighbor=src)`。

**关键约束**：只有 `NEIGHBOR_PROBE_RESPONSE` 能建立 direct neighbor；普通 mesh frame 的 `src` 不用于自动推断直连关系。

## 5. LINK_STATE 上报

`mesh_node_runtime_send_link_state_to_upstream()`（`mesh_node_runtime.c:558`）构造 `LINK_STATE`：

- `src` = 本机地址
- `dst` = `control_plane_addr`（主机）
- `neighbor` = 刚学到的直连邻居
- `link_up = 1`，`quality = 1`
- `local_port` = 本机发往该邻居应使用的本地端口

该帧通过 `send_frame_to_port(upstream_port)` 直接发到上游物理口，不查路由表。

## 6. 下游 REGISTER 中继行为

当已分配地址的从机收到 `REGISTER(src=0xFF, dst=0xFF)` 时（`mesh_node_runtime.c:1027`）：

1. 解析 REGISTER。
2. 在 `pending_bootstrap` 表中记录 `uid + boot_nonce -> ingress_port`（最多 4 条）。
3. 将原始 REGISTER 原样转发到 `upstream_port`。

当主机的 `ASSIGN` 沿原路返回时：

1. 根据 UID 命中 `pending_bootstrap`。
2. 通过 `send_frame_to_port(pending.ingress_port)` 把 ASSIGN 转给下游。
3. 学习 `payload.node_addr -> pending.ingress_port`。
4. 在本地 direct route 中写入 `dst=node_addr, next_hop=node_addr`。
5. 向主机上报 `LINK_STATE(src=本机, neighbor=node_addr)`。
6. 清理 pending 表项。

## 7. addr -> port 学习

动态映射表位于 `mesh_node_service`：

```c
struct mesh_node_service_addr_port {
    bool used;
    uint8_t mesh_addr;
    uint8_t port_id;
};
```

学习来源：

- `NEIGHBOR_PROBE_RESPONSE` 通过 `learn_peer_port` 回调写入。
- 下游 `ASSIGN` 回转后写入。

发送时，`mesh_node_service_send_frame()`（`mesh_node_service.c:196`）将 `next_hop`（mesh 地址）查到对应 UART 端口；查不到返回 `-MESH_ERR_NO_ROUTE`。

`MESH_NODE_SERVICE_NEIGHBOR_ANY`（`0xFF`）保留为广播选择器，用于初始 REGISTER 等需要发到所有端口的场景。

## 8. 关键 API

| 接口 | 说明 |
|------|------|
| `mesh_node_runtime_get_default_config()` | 填充默认配置：`local_addr=0xFF`，启用 `auto_register_on_init`。 |
| `mesh_node_runtime_init()` | 初始化 cluster、processor，发送首次 REGISTER。 |
| `mesh_node_runtime_deinit()` | 清理状态。 |
| `mesh_node_runtime_notify_link_up()` | 链路恢复通知，重发 REGISTER。 |
| `mesh_node_runtime_process_frame_from_port()` | 处理一帧并携带入口端口。 |
| `mesh_node_runtime_poll_once()` | 从 receive_frame 拉取一帧并处理；空闲时驱动重传。 |
| `mesh_node_runtime_format_routes()` | 输出本机地址、上游端口/邻居、direct-table 路由。 |
| `mesh_node_service_init()` | 初始化 UART 端口与 runtime。 |
| `mesh_node_service_poll_once()` | 轮询所有端口并处理最多一帧。 |
| `mesh_node_service_learn_addr_port()` | 手动写入/更新 `addr -> port` 映射。 |

## 9. 调试输出

- `/sys/routes` 调用 `mesh_node_runtime_format_routes()`，显示本机地址、上游端口/邻居、direct-table route。
- `/sys/uart` 调用 `mesh_node_service_format_uart_stats()`，显示每个 UART 端口的 DMA 位置、帧队列、坏帧/丢帧计数。

## 10. 相关文件

- `pwos-slave/User/mesh/mesh_node_runtime.c/.h`
- `pwos-slave/User/mesh/mesh_node_service.c/.h`
- `pwos-shared/mesh/envelope/mesh_protocal.h`：帧类型、payload 定义。
- `pwos-shared/mesh/processer/mesh_processer.h`：`MESH_PROCESSER_INGRESS_PORT_NONE`、callback 签名。
- `docs/adr/0004-relay-bootstrap.md`：下游 REGISTER 中继的 ADR。
