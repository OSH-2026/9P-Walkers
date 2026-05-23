# Mesh 模块整体说明（接口导向）

本文档聚焦 mesh 模块在系统中的两条关键接口边界：

1. 向上对 mini9P 模块暴露的数据面接口。
2. 向下对串口/链路层暴露的收发接口。

目标是把“谁负责什么”讲清楚，避免后续接入时出现重复封装或职责重叠。

## 1. 模块分层总览

当前 mesh 目录由三层组成：

- envelope：定义 mesh 信封协议（帧格式、控制面消息编解码、CRC）。
- processer：统一做收帧、解帧、路由分流、转发、本机控制分发。
- cluster：维护本机路由/拓扑状态，并提供处理器可直接绑定的回调接口。

推荐链路如下：

```text
串口(UART)/其他链路
        |
        v
receive_frame/send_frame 回调
        |
        v
mesh_processer (解封装、转发、分流)
   |                         |
   | 控制面                  | 数据面(MINI9P)
   v                         v
cluster_control_handler      mini9p_server/client_handler
```

## 2. 对 mini9P 模块的接口

### 2.1 数据面封装约定

mesh 使用 `MESH_TYPE_MINI9P` 承载 mini9P 原始帧字节流：

- `payload` = 完整 mini9P 帧（含 mini9P 自身头和 CRC）。
- mesh 中继节点只看 mesh 头，不解析 mini9P 内部语义。
- 仅目标节点在本机命中时，processor 才解析 mini9P 头并分流到 T*/R*。

对应 API（envelope 层）：

- `mesh_build_mini9p_frame`
- `mesh_parse_mini9p_payload`

### 2.2 processor 向 mini9P 的回调接口

processor 提供两条 mini9P 回调链：

1. `mini9p_server_handler`：处理本机收到的 T* 请求。
2. `mini9p_client_handler`：处理本机收到的 R* 响应。

接口定义位于 `mesh_processer.h`，核心语义如下：

- server 回调输入是 mini9P 原始请求帧，输出是 mini9P 原始响应帧。
- client 回调输入是 `m9p_frame_view`（已被 processor 解码）。
- processor 负责把 server 回调输出再封装成 mesh 帧回传请求源地址。

### 2.3 T*/R* 分流规则

当前分流规则由 processor 内置：

- `type & 0x80 == 0` 视为请求（T*）-> 走 `mini9p_server_handler`。
- `type & 0x80 != 0` 视为响应（R*）-> 走 `mini9p_client_handler`。

该规则与 mini9P 现有类型编码保持一致。

## 3. 对串口层（链路层）的接口

### 3.1 最小链路接口定义

processor 不直接依赖 UART 驱动，而是依赖两条回调：

1. `mesh_processer_send_frame_fn`
2. `mesh_processer_receive_frame_fn`

这两条回调就是 mesh 与串口层的正式边界。

### 3.2 send_frame 语义

`send_frame(transport_ctx, next_hop, tx_data, tx_len)`

- `next_hop` 由 route_lookup 给出，代表应投递到的下一跳地址。
- `tx_data` 为完整 mesh 帧字节序列，不需要串口层再做任何协议改写。
- 返回 `0` 表示发送成功，负值表示链路层错误。

串口层建议：

- 仅负责可靠发送，不改写 mesh 头。
- 若底层有端口映射，可在 `transport_ctx` 内完成地址到端口的映射。

### 3.3 receive_frame 语义

`receive_frame(transport_ctx, rx_data, rx_cap, rx_len)`

- 成功时返回 `0` 并输出一帧完整 mesh 帧。
- 失败/暂未收齐时返回负值（例如可重试错误）。
- 建议底层处理好帧边界，processor 假设拿到的是一帧完整数据。

### 3.4 poll 与上层线程模型

processor 提供两种接线方式：

1. 外层自行收帧：调用 `mesh_processer_process_frame`。
2. processor 主动轮询：调用 `mesh_processer_poll_once`。

典型裸循环：

```c
for (;;) {
    (void)mesh_processer_poll_once(&processor);
}
```

## 4. route_lookup/control_handler 与 cluster 的对接

cluster 已提供与 processor 完全同签名的适配函数：

- `cluster_processor_route_lookup`
- `cluster_processor_control_handler`

推荐直接绑定：

```c
processor_cfg.cluster_ctx = &cluster;
processor_cfg.route_lookup = cluster_processor_route_lookup;
processor_cfg.control_handler = cluster_processor_control_handler;
processor_cfg.control_handler_ctx = NULL; /* 回退到 cluster_ctx */
```

行为说明：

- 非本机帧：processor 调 `route_lookup` 获取 next_hop 后转发。
- 本机控制帧：processor 调 `control_handler`，必要时发送控制回包。
- 本机 mini9P 帧：processor 按 T*/R* 分流给 mini9P 回调。

## 5. 推荐接入顺序

建议按以下顺序渐进接入：

1. 先接 `send_frame/receive_frame`，确保可收发完整 mesh 帧。
2. 接入 `route_lookup`，先打通中继转发链路。
3. 接入 `control_handler`，让 REGISTER/ROUTE_UPDATE/PING 生效。
4. 最后接入 mini9P server/client 回调，打通文件请求与响应链路。

这样做可以快速定位问题来源：

- 链路问题：在步骤 1 暴露。
- 路由问题：在步骤 2 暴露。
- 控制面问题：在步骤 3 暴露。
- 业务协议问题：在步骤 4 暴露。

## 6. 常见实现注意点

1. hop 必须在转发时递减，耗尽应丢帧。
2. 控制回包必须是完整 mesh 帧，processor 不会二次补头。
3. mini9P server 回调输出的是 mini9P 帧，不是 mesh 帧。
4. `local_addr` 未分配阶段建议保持 `0xFF`，并优先完成 ASSIGN 流程。
5. 串口层不要解析 mini9P 或控制负载，避免分层耦合。
