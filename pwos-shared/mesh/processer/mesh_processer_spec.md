# Mesh Processor 接口文档（v1）

## 1. 角色定位

mesh processor 是 mesh 层的中间分发器，负责把“原始链路上的字节流”变成“按语义分流的处理动作”。

它不直接实现以下内容：

- 不直接维护全局拓扑图。
- 不直接实现 cluster 的路由计算。
- 不直接实现 mini9P client/server 的具体业务。
- 不直接绑定 UART / SPI / 其他物理链路。

它只做四件事：

1. 收到一帧 mesh 数据。
2. 解析 mesh envelope。
3. 判断是否发给自己。
4. 对本机帧做控制面/数据面分流，对非本机帧做转发。

## 2. 处理模型

### 2.1 外部拉帧模式

适用场景：

- 外层已经有独立串口任务。
- 外层希望先做缓存、重同步或流控，再交给 processor。

流程：

1. 先从物理链路收一帧完整 mesh 数据。
2. 调用 `mesh_processer_process_frame()`。
3. processor 内部完成解帧、分流、转发或本地处理。

### 2.2 轮询模式

适用场景：

- 想让 processor 自己在循环里拉帧。
- 想快速做单线程/单任务原型。

流程：

1. 调用 `mesh_processer_poll_once()`。
2. processor 通过 `receive_frame()` 拉一帧。
3. `receive_frame()` 同时返回物理 `ingress_port`（无端口概念时为 `MESH_PROCESSER_INGRESS_PORT_NONE`）。
4. processor 再调用统一的 `process_frame_from_port()` 入口。

## 3. 初始化与配置

### 3.1 必填回调

`mesh_processer_init()` 至少需要以下三个回调：

- `send_frame`：把一帧发到指定下一跳。
- `receive_frame`：从链路上收一帧。
- `route_lookup`：询问 cluster 某个目的地址是本机还是下一跳。

如果缺少其中任意一个，初始化会失败。

### 3.2 可选回调

以下回调可先置空，方便分阶段接入：

- `control_handler`
- `mini9p_server_handler`
- `mini9p_client_handler`

当它们未配置时，processor 不会崩溃，只会把对应帧当成“已消费但未落地”。

### 3.3 本机地址

`local_addr` 是 processor 判断“是不是发给自己”的依据。

- 正常运行时应是已分配的 mesh 短地址。
- bootstrap 阶段可以先用 `0xFF`。
- 若本机地址尚未确定，processor 仍能做转发，但本机命中判断不会正常生效。

### 3.4 默认 hop

`default_hop` 用于：

- 本机发起或回包时的默认跳数。
- 控制面回复和 mini9P 回复的重封装。

若设置为 0，初始化会自动回退到默认值 `MESH_PROCESSER_DEFAULT_HOP`。

### 3.5 control_handler 上下文选择

processor 调用 `control_handler` 时，上下文按以下顺序选择：

1. 若 `control_handler_ctx` 非空，优先使用它。
2. 否则回退到 `cluster_ctx`。

这样可以兼顾两类场景：

- 常见接法：只填 `cluster_ctx`，直接对接 cluster。
- 特殊接法：控制面处理与路由查询分属不同对象，单独指定 `control_handler_ctx`。

## 4. API 说明

### 4.1 `mesh_processer_get_default_config()`

作用：生成一个清零后的默认配置。

行为：

- `local_addr` 设为 `0xFF`。
- `default_hop` 设为默认 hop 值。
- 其余回调清零。

适合用法：

```c
struct mesh_processer_config config;
mesh_processer_get_default_config(&config);
config.send_frame = ...;
config.receive_frame = ...;
config.route_lookup = ...;
```

### 4.2 `mesh_processer_init()`

作用：初始化 processor 实例。

输入：

- `processor`：待初始化对象。
- `config`：运行时配置。

返回值：

- `0`：成功。
- 负值：失败。

初始化后：

- `initialized = true`。
- 内部收发缓冲区被清零。

### 4.3 `mesh_processer_deinit()`

作用：清理 processor。

行为：

- 清零整个结构体。
- 适合断链、重建、销毁时调用。

### 4.4 `mesh_processer_process_frame()`

作用：处理一帧已经收进来的 mesh 数据。

处理顺序：

1. 解 mesh envelope。
2. 询问 `route_lookup`。
3. 若目的地址不是本机，则转发。
4. 若目的地址是本机，则按消息类型分发。

本机分发规则：

- 控制类型：交给 `control_handler`。
- `MESH_TYPE_MINI9P`：继续解 mini9P，T* 交给 `mini9p_server_handler`，R* 交给 `mini9p_client_handler`。

### 4.5 `mesh_processer_process_frame_from_port()`

作用：处理一帧已经收进来的 mesh 数据，并显式携带该帧的入口端口。

语义：

- `ingress_port` 来自 `receive_frame()` 的 `out_ingress_port`。
- 没有端口概念的调用方应传 `MESH_PROCESSER_INGRESS_PORT_NONE`。
- processor 自身不解释端口，只负责保留这条信息供上层 runtime/control handler 使用。

### 4.6 `mesh_processer_poll_once()`

作用：从底层链路拉一帧，然后处理。

适合：

- 循环任务。
- 快速原型。
- 单链路场景。

## 5. 回调契约

### 5.1 `mesh_processer_send_frame_fn`

输入：

- `next_hop`：cluster 已查到的下一跳。
- `tx_data/tx_len`：完整 mesh 帧。

要求：

- 必须把整帧完整送出。
- 不关心帧内部语义。

### 5.2 `mesh_processer_receive_frame_fn`

要求：

- 每次返回一帧完整的 mesh 数据。
- 通过 `out_ingress_port` 返回实际入口端口；无端口概念时返回 `MESH_PROCESSER_INGRESS_PORT_NONE`。
- 不足一帧时建议返回可重试错误码。

### 5.3 `mesh_processer_route_lookup_fn`

输入：

- `dst`：目的地址。

输出：

- `out_is_local = true`：本机命中。
- `out_is_local = false`：需要转发到 `out_next_hop`。

### 5.4 `mesh_processer_control_handler_fn`

用于处理控制面消息。

推荐承担的业务：

- REGISTER：记录临时反向路径、更新节点在线状态。
- ASSIGN：更新本机 node_addr / node_name。
- ROUTE_UPDATE：刷新路由版本与下一跳。
- PING / TIME_SYNC：更新在线状态和时延统计。

输出要求：

- 不需要回包时，`*out_reply_len = 0`。
- 需要回包时，`out_reply_frame` 必须是完整 mesh 帧。

### 5.5 `mesh_processer_mini9p_server_handler_fn`

用于处理本机收到的 mini9P T* 请求。

典型接法：

- 直接调用 `m9p_server_handle_frame()`。
- 将响应写入 `out_response_data`。

### 5.6 `mesh_processer_mini9p_client_handler_fn`

用于处理本机收到的 mini9P R* 响应。

典型接法：

- 交给等待请求的 client 状态机。
- 或者写入 pending 队列。

## 6. 错误语义

processor 主要返回以下几类错误：

- `MESH_ERR_BAD_FRAME`：帧格式非法、CRC 错误、解帧失败。
- `MESH_ERR_INVALID_STATE`：processor 未初始化或缺少必填回调。
- `MESH_ERR_NO_ROUTE`：hop 用尽或路由查询失败后不可转发。
- `MESH_ERR_UNSUPPORTED_TYPE`：收到了当前 processor 不认识的本机类型。

## 7. 处理分支

### 7.1 非本机帧

若 `dst != local_addr`：

1. 调用 `route_lookup`。
2. 如果是本机路由，直接结束。
3. 如果不是本机路由，hop 减 1。
4. 重新封装或直接发送。

### 7.2 本机控制帧

若 `dst == local_addr` 且 type 是控制面类型：

1. 交给 `control_handler`。
2. 如果 handler 产出回包，则直接发回网络。

### 7.3 本机 mini9P 帧

若 `dst == local_addr` 且 type 是 `MESH_TYPE_MINI9P`：

1. 先解出内层 mini9P 帧。
2. 如果是 T*，交给 server。
3. 如果是 R*，交给 client。

## 8. 接入建议

建议接入顺序如下：

1. 先让 `send_frame / receive_frame / route_lookup` 跑通。
2. 再接 `control_handler`。
3. 再接 `mini9p_server_handler`。
4. 最后接 `mini9p_client_handler`。

这样可以先验证 mesh 转发链路，再逐步把控制面和文件协议挂上来。

## 9. 与 cluster 的直接接线

当前 cluster 已提供与 processor 完全同签名的适配函数：

- `cluster_processor_route_lookup`
- `cluster_processor_control_handler`

最小接线示例：

```c
processor_cfg.cluster_ctx = &g_cluster;
processor_cfg.route_lookup = cluster_processor_route_lookup;
processor_cfg.control_handler = cluster_processor_control_handler;
processor_cfg.control_handler_ctx = NULL; /* 自动回退到 cluster_ctx */
```

这种接法的优点是：

- processor 层无需额外胶水代码。
- cluster 的路由查询和控制面落地语义统一由一个对象管理。

## 10. 与 cluster / mini9P 的边界

processor 不负责：

- 节点名分配策略。
- 全局拓扑图维护。
- mini9P 的 fid 生命周期。
- 具体文件树内容。

processor 负责：

- 把控制帧交给 cluster。
- 把 mini9P T* / R* 交给对应处理器。
- 把非本机帧按路由转发。

## 11. 备注

这个 processor 当前是“最小可用中间层”，后续如果你们决定把它升级为：

- 带缓存的重传代理，
- 支持多链路负载均衡，
- 或支持更复杂的控制面状态机，

也可以在不改现有接口的情况下继续扩展回调内容。
