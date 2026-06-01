# pwos-shared/mesh

OSH-2026 MCU 集群项目的共享 mesh 层源码。负责在 UART/串口链路上实现 mesh envelope 协议、节点注册/分配、拓扑维护、路由计算与多跳转发，同时为 mini9P 数据面提供承载通道。

## 模块概览

```
串口/UART 链路
    │
    ▼
transport/          # UART transport 适配层（ESP32/STM32 HAL / POSIX）
    │
    ▼
processer/ # 帧分发器：解封装 →路由查询 → 转发 / 控制面 / mini9P 分流
    │
    ├──► cluster/ # 拓扑/路由管理层（DIRECT_TABLE / TOPOLOGY 两种模式）
    │
    └──► mini9P      # 数据面：MESH_TYPE_MINI9P 承载完整 mini9P 帧
```

```
envelope/            # mesh帧格式、CRC、控制面编解码
processer/           # mesh_processer：中间分发层
cluster/             # cluster：拓扑/路由状态
transport/           # mesh_uart_transport：链路适配
mesh_host_vfs_spec.md # 主机侧 VFS 桥接说明
mesh_overview_spec.md # 接口边界与接入顺序
```

## 子模块说明

### envelope —协议格式

`mesh_protocal.c/.h` 实现 mesh envelope 帧的编解码：

| 字段 | 长度 | 说明 |
|---|---|---|
| Magic | 2B | 固定 `MH`，用于流重同步 |
| FrameLen | 2B | Version 到 Payload 末尾字节数 |
| Version | 1B | 协议版本，v1 =0x01 |
| Type | 1B | 消息类型 |
| Src/Dst | 2B | 源/目标 mesh 短地址 |
| Seq | 2B | 请求序号 |
| Hop | 1B | 剩余跳数（TTL） |
| Flags | 1B | CONTROL / NEEDS_ACK / IS_RETRY |
| Payload | N | 控制面或 mini9P 数据面 |
| CRC16 | 2B | CRC-16/CCITT-FALSE |

**控制面类型（0x10+）**：

| 类型 | 值 | 用途 |
|---|---|---|
| REGISTER | 0x10 | bootstrap 注册请求 |
| ASSIGN | 0x11 | 主机分配地址和节点名 |
| PING/PONG | 0x12/0x13 | 探活 |
| TIME_SYNC | 0x14 | 四时间戳时钟同步 |
| ROUTE_UPDATE | 0x15 | 路由项更新 |
| LINK_STATE | 0x16 | 邻居链路状态上报 |
| NEIGHBOR_PROBE_REQUEST | 0x17 | 单链路本地邻居探测 |
| NEIGHBOR_PROBE_RESPONSE | 0x18 | 单链路本地邻居响应 |
| ERROR | 0x7F | 错误上报 |

**核心约定**：

- 中继只解析 envelope 头，不解析 mini9P payload
- `0xFF` 表示未分配地址（bootstrap 阶段）
- 控制面消息设置 `MESH_FLAG_CONTROL`，数据面清除该标志

### processer — 帧分发层

`mesh_processer.c/.h` 是 mesh 层的中间分发器，把"原始链路字节流"变为"按语义分流的处理动作"。

**三条必填回调**：

```c
send_frame(transport_ctx, next_hop, tx_data, tx_len)
receive_frame(transport_ctx, rx_data, rx_cap, rx_len, out_ingress_port)
route_lookup(cluster_ctx, dst, out_is_local, out_next_hop)
```

**两条可选回调**：

```c
control_handler() // 控制面消息处理
mini9p_server_handler() // mini9P T* 请求处理
mini9p_client_handler() // mini9P R* 响应处理
```

**分流规则**：

- `dst != local_addr` → 调用 `route_lookup` 转发，hop--
- `dst == local_addr && CONTROL` → 交给 `control_handler`
- `dst == local_addr && MINI9P` → 解出 mini9P 帧，T* 走 server，R* 走 client

**两种工作模式**：

1. **外部拉帧**：`mesh_processer_process_frame_from_port()` — 外层收帧后调用
2. **轮询模式**：`mesh_processer_poll_once()` — processor 自己调 `receive_frame` 收帧

**与 cluster 的接线**：

cluster 已提供同签名适配函数，可直接绑定：

```c
processor_cfg.cluster_ctx = &g_cluster;
processor_cfg.route_lookup = cluster_processor_route_lookup;
processor_cfg.control_handler = cluster_processor_control_handler;
```

### cluster — 拓扑与路由

`cluster.c/.h` 是 mesh 控制面里的"节点与路由管理层"，回答三类问题：

1. 目标是不是本机
2. 如果不是，下一跳是谁
3. 路由更新时如何刷新本地状态

**两种工作模式**：

| 模式 | 适用场景 |
|---|---|
| DIRECT_TABLE | 子机 / 简单中继 / 主机已算好 next_hop |
| TOPOLOGY | 主机 / 需要路径计算的场景 |

**TOPOLOGY 模式**（主机）维护链路，再从拓扑派生路由表；DIRECT_TABLE 模式（从机）直接维护 `dst → next_hop` 表项。

**核心 API**：

```c
cluster_init(&cluster, &config); // 初始化
cluster_set_local_addr(&cluster, addr); // 更新本机地址
cluster_add_route(&cluster, dst, next_hop, metric); // 插入/更新路由
cluster_add_link(&cluster, from, to, metric, bidirectional); // 添加拓扑边
cluster_lookup_next_hop(&cluster, dst, &is_local, &next_hop); // 查询 next_hop
cluster_can_reach(&cluster, dst, &reachable); // 可达性查询
cluster_apply_route_update(&cluster, &payload); // 处理 ROUTE_UPDATE 帧
cluster_rebuild_routes(&cluster); // 从拓扑重算路由（TOPOLOGY 模式）
```

**只读路径计算**（主机 controller sync）：

```c
cluster_compute_next_hop_from(&cluster, source, dst, &next_hop, &metric);
```

- 不写入 `cluster->routes`，只做即时计算
- 用于 LINK_STATE 后主机向下发 ROUTE_UPDATE

### transport — UART 适配层

`mesh_uart_transport.c/.h` 适配不同平台 UART：

|平台 | 宏 |
|---|---|
| ESP-IDF | `MESH_UART_TRANSPORT_USE_ESP_IDF` |
| STM32 HAL | `MESH_UART_TRANSPORT_USE_STM32_HAL` |
| POSIX | 默认 |

**接口**：

```c
mesh_uart_transport_init(&transport, &config);
mesh_uart_transport_send_frame(&transport, data, len);
mesh_uart_transport_receive_frame(&transport, rx_buf, rx_cap, rx_len);
mesh_uart_transport_rx_pending(&transport);  // 是否有待处理数据
mesh_uart_transport_flush_input(&transport);  // 丢弃残帧
```

**receive_frame 语义**：每次返回一帧完整 mesh 数据，不足一帧时返回可重试错误码（`-MESH_ERR_BUSY`）。

## 上层接线关系

```
pc_master_emulator / mesh_host_runtime
    │
    ├─► mesh_processer（帧分发）
    │       ├─► cluster（拓扑/路由）
    │       │       └─► cluster_config（→ cluster_vfs → /mcuN/... 命名空间）
    │       └─► mini9p_client / mini9p_server（数据面）
    │
    └─► cluster_vfs（节点名 <-> UID 映射、attach状态）
```

**主机侧发现/离线流程**：

- bootstrap REGISTER → 主机发 ASSIGN；ASSIGN 成功后或收到已分配地址 REGISTER → `cluster_config_on_mesh_node_registered()` → 同步 online 位、VFS UID 映射和 mesh-backed Mini9P client，状态 = NEW
- 节点 LINK_STATE → cluster 添加拓扑边 → `cluster_config_refresh_all_nodes_connectivity()` → VFS 刷新可达性
- 节点离线 → `cluster_config_on_node_departed()` → cluster 摘图，VFS 保留 UID 映射，清除地址绑定，9P 状态回退 NEW

**从机侧流程**：

- 上电 → broadcast REGISTER(src=0xFF)
- 收到 ASSIGN → 更新本机地址，记录 upstream_port；主机在 ASSIGN 成功发出后即注册该 UID/addr
- ASSIGN 后 → 向所有端口广播 NEIGHBOR_PROBE_REQUEST
- 收到 NEIGHBOR_PROBE_RESPONSE → 学到 `src → ingress_port`，若已知上游 control-plane 则上报 LINK_STATE

## 关键设计约束

1. **UID 是节点身份主键**，不是 mesh短地址。mesh 地址可重分配，UID永久稳定
2. **拓扑有向**：收到 `A → B` 不自动补 `B → A`；反向路径必须由 B自行上报 LINK_STATE
3. **mini9P 是 payload**：mesh 中继不解析 mini9P 内容，只做 envelope 头的转发
4. **route_lookup 是核心接口**：processor 只依赖"本机还是转发"这个查询结果，不关心路由怎么来的
5. **离线保留映射**：节点离线后不删除 UID → 名字映射，只清除在线绑定和 9P 状态

## 接入顺序建议

1. 先接 `send_frame / receive_frame / route_lookup`，确保可收发完整 mesh 帧
2. 接 `control_handler`，让 REGISTER/ASSIGN/ROUTE_UPDATE生效
3. 接 `mini9p_server_handler / mini9p_client_handler`，打通文件请求链路

这样做问题来源清晰：链路问题在步骤 1 暴露，路由问题在步骤 2，控制面在步骤 3，业务协议在步骤 4。

## 文件清单

```
mesh/
├── envelope/
│   ├── mesh_protocal.c      # 帧编解码、CRC、build/parse 函数
│   └── mesh_protocal.h       # 类型定义、标志位、消息类型枚举
├── processer/
│   ├── mesh_processer.c     # 帧分发、T*/R* 分流、转发
│   ├── mesh_processer.h # 配置结构、回调签名、API声明
│   └── mesh_processer_spec.md
├── cluster/
│   ├── cluster.c            # 拓扑/路由管理、route_lookup 实现
│   ├── cluster.h            # 配置、数据模型、API 声明
│   └── mesh_cluster_spec.md
├── transport/
│   ├── mesh_uart_transport.c  # 多平台 UART 适配
│   └── mesh_uart_transport.h
├── mesh_overview_spec.md    # 接口边界、接线关系、接入顺序
├── mesh_host_vfs_spec.md # 主机侧 VFS 桥接语义
└── module test(on PC)/     # PC 端冒烟测试
```
