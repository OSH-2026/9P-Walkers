# Distributed MCU Cluster OS - 工程架构与开发规划

## 1. 总体架构

当前项目已经演进为**主控（ESP32-P4 / PC 模拟器）+ 多 STM32 从机**的集群系统，核心通信层是 `pwos-shared/mesh` 提供的 mesh envelope：

- mini9P 数据面帧被封装为 `MESH_TYPE_MINI9P` 的 payload；
- 控制面消息（REGISTER、ASSIGN、ROUTE_UPDATE、LINK_STATE 等）通过同一 envelope 传输；
- 中继节点只解析 envelope 头，不解析 mini9P 内容；
- 主机维护拓扑并派生路由，从机维护简化 direct-table 路由表。

```text
Shell / Lua / WebShell
        │
        ▼
  cluster_vfs          # /mcuN/... 统一命名空间、UID↔名字映射、attach 状态
        │
        ▼
  mini9p_client        # 主机侧 9P 请求/响应 client
        │
        ▼
  mesh_host_runtime    # 处理 REGISTER/LINK_STATE/ROUTE_UPDATE，绑定 mesh-backed client
        │
        ▼
  mesh_processer       # 帧分流：转发 / 控制面 / mini9P 数据面
        │
        ├──► cluster   # 拓扑/路由管理（主机 TOPOLOGY / 从机 DIRECT_TABLE）
        │
        └──► mini9p_server / mini9p_client
        │
        ▼
  mesh_uart_transport  # UART DMA + IDLE 中断 + frame queue（STM32）/ ESP-IDF / POSIX
  mesh_wifi_link       # ESP32-P4 侧 TCP/9000 WiFi 透传接入
        │
        ▼
  mesh_node_runtime    # 从机侧 REGISTER/ASSIGN/NEIGHBOR_PROBE/LINK_STATE 处理
        │
        ▼
  mini9p_server        # 从机侧 9P 请求处理
        │
        ▼
  local_vfs backend    # littleFS / sys / dev / 计算任务回调
```

## 2. 仓库目录结构

```
9P-Walkers/
│
├── docs/
│   ├── architecture.md              # 本文件
│   ├── protocol_spec.md             # mini9P payload 规范
│   ├── mesh_envelope_spec.md        # mesh envelope 协议规范
│   ├── mesh_wifi_link.md            # WiFi 链路说明
│   ├── slave_mesh_runtime.md        # 从机 runtime 开发者指南
│   ├── build_and_flash.md           # 编译烧录指南
│   ├── adr/                         # 架构决策记录
│   ├── 调研报告.md
│   └── 可行性分析.md
│
├── pwos-master-esp32p4/             # ESP32-P4 主控固件
│   ├── main/                        # app_main、Lua 绑定、VFS 绑定
│   ├── cluster/                     # cluster_config：mesh 事件 → VFS 状态
│   ├── mesh/                        # mesh_host_runtime / mesh_host_service / mesh_wifi_link
│   ├── vfs_bridge/                  # cluster_host_vfs
│   ├── shell/                       # 本地 C Shell
│   ├── web/                         # HTTP + WebSocket + WiFi softAP
│   └── build/                       # ESP-IDF 构建输出
│
├── pwos-slave/                      # STM32F407 从机工程
│   ├── Core/                        # STM32 HAL 生成的 Core 代码
│   ├── Drivers/                     # HAL 驱动
│   └── User/                        # 用户业务代码
│       ├── app/
│       │   └── mesh_node_mini9p_init.c   # 从机组装入口
│       ├── mesh/                    # mesh_node_runtime / mesh_node_service
│       ├── fs/                      # littleFS port、local_vfs
│       ├── dev/                     # 外设回调
│       └── compute/                 # 计算任务后端
│
├── pwos-slave-stm32f411/            # STM32F411 从机变体
│   └── User/app/mesh_node_mini9p_init.c   # 仅适配 UART handle 与端口配置
│
├── pwos-shared/                     # 主从共享代码
│   ├── mini9p/                      # mini9P 协议本体
│   │   ├── mini9p_protocol.c/.h
│   │   ├── mini9p_client.c/.h
│   │   ├── mini9p_server.c/.h
│   │   └── README.md
│   └── mesh/                        # mesh envelope 与路由层
│       ├── envelope/
│       │   ├── mesh_protocal.c/.h
│       │   └── mesh_protocol_spec.md
│       ├── processer/
│       │   ├── mesh_processer.c/.h
│       │   └── mesh_processer_spec.md
│       ├── cluster/
│       │   ├── cluster.c/.h
│       │   └── mesh_cluster_spec.md
│       ├── transport/
│       │   ├── mesh_uart_transport.c/.h
│       │   └── (transport_abstraction.md)
│       ├── mesh_overview_spec.md
│       ├── mesh_host_vfs_spec.md
│       └── module test(on PC)/
│
└── tools/pc_master_emulator/        # PC 端主控模拟器
    ├── main.c
    └── README.md
```

## 3. 各层职责

### 3.1 Mesh Envelope 层

位置：`pwos-shared/mesh/envelope/`

职责：
- 定义 mesh 帧格式（Magic、FrameLen、Version、Type、Src/Dst、Seq、Hop、Flags、Payload、CRC16）。
- 实现帧编解码与 CRC-16/CCITT-FALSE 校验。
- 实现控制面 payload 的构造/解析。

关键文件：
- `mesh_protocal.h`：类型、常量、payload 结构体。
- `mesh_protocal.c`：编码/解码/校验函数。

### 3.2 Mesh Processor 层

位置：`pwos-shared/mesh/processer/`

职责：
- 从链路收帧、解帧、校验。
- 判断目标地址是否为本机。
- 非本机帧：查询 cluster 下一跳，`hop--` 后转发。
- 本机控制帧：交给 `control_handler`。
- 本机数据帧（`MESH_TYPE_MINI9P`）：解出 mini9P，T* 给 server，R* 给 client。

关键文件：
- `mesh_processer.h/c`

### 3.3 Cluster 层

位置：`pwos-shared/mesh/cluster/`

职责：
- 维护节点表、路由表、拓扑链路表。
- 提供 `route_lookup` 给 processor。
- 主机 TOPOLOGY 模式下从拓扑派生路由；从机 DIRECT_TABLE 模式下直接查表。

关键文件：
- `cluster.h/c`

### 3.4 Transport 层

位置：`pwos-shared/mesh/transport/`

职责：
- 屏蔽 UART 平台差异（ESP-IDF / STM32 HAL / POSIX）。
- STM32 侧已实现 DMA + IDLE 中断 + per-port frame queue。
- 每次 `receive_frame()` 返回一帧完整 mesh 数据。

关键文件：
- `mesh_uart_transport.h/c`

### 3.5 主机 Runtime / Service

位置：`pwos-master-esp32p4/mesh/`

职责：
- `mesh_host_runtime`：处理 REGISTER、LINK_STATE、ROUTE_UPDATE，维护 mesh-backed mini9P client，等待 R* 时继续处理控制帧。
- `mesh_host_service`：管理最多 4 个 UART 端口，启动默认后台任务。
- `mesh_wifi_link`：监听 TCP/9000，让 WiFi 透传模块以与 UART 同构的方式收发 mesh 帧。

关键文件：
- `mesh_host_runtime.c/h`
- `mesh_host_service.c/h`
- `mesh_wifi_link.c/h`

### 3.6 主机 VFS 桥接

位置：`pwos-master-esp32p4/vfs_bridge/`

职责：
- `cluster_host_vfs`：把 UID 映射为 `/mcuN` 名字，维护 9P attach 状态，按 cluster 可达性刷新在线状态。
- `cluster_config`：把 mesh 控制面事件翻译给 VFS。

关键文件：
- `cluster_host_vfs.c/h`
- `cluster_config.c/h`

### 3.7 从机 Runtime / Service

位置：`pwos-slave/User/mesh/`

职责：
- `mesh_node_runtime`：bootstrap REGISTER、ASSIGN 落地、邻居探测、direct-table 路由、Mini9P 分流。
- `mesh_node_service`：多 UART 端口管理、`addr -> port` 学习表、轮询收包。
- `mesh_node_mini9p_init`：板级组装入口。

关键文件：
- `mesh_node_runtime.c/h`
- `mesh_node_service.c/h`
- `User/app/mesh_node_mini9p_init.c`

### 3.8 Mini9P 层

位置：`pwos-shared/mini9p/`

职责：
- 文件访问语义：attach/walk/open/read/write/stat/clunk。
- 主机侧 client、从机侧 server。
- 在线缆上**不直接出现**，而是作为 mesh `MESH_TYPE_MINI9P` 的 payload。

关键文件：
- `mini9p_protocol.h/c`
- `mini9p_client.h/c`
- `mini9p_server.h/c`

## 4. 控制面与数据面流转

### 4.1 新节点上线（从机上电）

```text
从机（addr=0xFF）
    │ broadcast REGISTER(src=0xFF, dst=0xFF)
    ▼
中继 A（如有）: 记录 uid+ingress_port，转发上游
    ▼
主机
    │ 识别 UID，分配/复用地址，发送 ASSIGN(dst=0xFF)
    ▼
从机
    │ 更新本地地址，记录 upstream_port
    │ broadcast NEIGHBOR_PROBE_REQUEST
    ▼
邻居回复 NEIGHBOR_PROBE_RESPONSE
    │ 学习 direct neighbor，若已知上游则上报 LINK_STATE
    ▼
主机收到 LINK_STATE → 更新拓扑 → 下发 ROUTE_UPDATE
```

### 4.2 文件访问

```text
Shell/Lua/WebShell
    │ cat /mcu1/sys/health
    ▼
cluster_vfs: 解析 target=mcu1，本地 path=/sys/health
    ▼
mini9p_client → mesh_host_runtime_client_request
    │ 构造 MESH_TYPE_MINI9P，src=host, dst=mcu1_addr
    ▼
mesh_processer → route_lookup → 转发或本地处理
    ▼
从机 mesh_node_runtime 收到 → 解出 mini9P Tread
    ▼
mini9p_server → local_vfs backend → 构造 Rread
    ▼
回包经 mesh MINI9P envelope 返回主机
```

## 5. 关键设计约束

1. **UID 是节点身份主键**，不是 mesh 短地址。mesh 地址可重分配，UID 永久稳定。
2. **拓扑有向**：收到 `A → B` 不自动补 `B → A`；反向路径必须由 B 自行上报 LINK_STATE。
3. **mini9P 是 payload**：mesh 中继不解析 mini9P 内容，只做 envelope 头转发。
4. **route_lookup 是核心接口**：processor 只依赖“本机还是转发”这个查询结果，不关心路由怎么来。
5. **离线保留映射**：节点离线后不删除 UID → 名字映射，只清除在线绑定和 9P 状态。
6. **单事务同步模型**：当前 `mesh_host_runtime_client_request` 仍是单事务，等待 R* 期间继续消费控制帧，但同一时刻只允许一个同步 Mini9P 请求占用接收链路。

## 6. 开发路线（当前状态）

已落地：
- [x] mini9P v1 文件协议
- [x] mesh envelope v1 + 控制面消息
- [x] 主机/从机 runtime
- [x] 多跳路由与拓扑维护
- [x] PC 模拟器 smoke test
- [x] STM32 UART DMA + IDLE 中断 + frame queue
- [x] ESP32-P4 WiFi 透传链路（mesh_wifi_link）

进行中 / 待完善：
- [ ] host runtime 并发请求模型（当前单事务）
- [ ] 主机侧为远端主动发来的 mini9P T* 提供完整本地 server handler
- [ ] 邻居传播路由模型评估
- [ ] 拓扑有向/双向语义最终确定
- [ ] VFS list_routes、自动重连、离线恢复策略

## 7. 相关文档

- `docs/protocol_spec.md`：顶层协议栈说明（mesh envelope + mini9P payload）
- `docs/mesh_envelope_spec.md` / `pwos-shared/mesh/envelope/mesh_protocol_spec.md`：mesh envelope 协议规范
- `docs/mesh_wifi_link.md`：ESP32-P4 主机侧 WiFi 透传链路
- `docs/slave_mesh_runtime.md`：从机 runtime 开发者指南
- `docs/cluster_config_usage.md`：`cluster_config` 与 VFS 的协作流程
- `docs/transport_abstraction.md`：UART/WiFi transport 抽象与 STM32 DMA 设计
- `docs/build_and_flash.md`：编译烧录指南
- `pwos-shared/mesh/cluster/mesh_cluster_spec.md`：cluster 数据模型与 API
- `pwos-shared/mesh/processer/mesh_processer_spec.md`：processor 回调与分流
- `pwos-shared/mesh/mesh_host_vfs_spec.md`：主机侧 VFS 桥接语义
- `pwos-shared/RUN_TIME_NODE_MESH.md`：主机侧 runtime 调用指南
- `pwos-master-esp32p4/vfs_bridge/design.md`：`cluster_host_vfs` 设计说明
- `tools/pc_master_emulator/README.md`：PC 串口联调指南
