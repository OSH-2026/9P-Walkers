我们现在打算实现动态注册节点，考虑串口相连的网状链路。
- 在路由表层面，我们有两个方向。
 - 第一是维持一张连接网，这样可以在主机侧了解全局路径连接，并且搜索最优路径，中间节点故障之后可以迅速找新路径。
 - 第二是维持一张表，记录节点名和通向该节点的下一跳的节点（使得跳数最少），然后由下一跳解析后续路径。
这里考虑的问题还包括，是否所有节点都维持全局节点的网或者表？
- 在9p协议改造方向，我们有三个激进程度不同的改造
 - 一个方向是，在9p协议的头部，加一个请求头，然后定义一种专用于节点连接的协议。对于server方,从usart的数据先经过一个中间层，如果发现该数据是属于节点网络信息的，则进行节点注册逻辑。如果属于9p,再接入9pserver。对于client方，根据串口电平状态，判断是否有节点接入，从而调用新协议的client发送节点网络信息获取请求。
 - 另一个方向是，扩展9p协议，新增一种9p信息类型。解析逻辑装进9pclient和server里面。请求逻辑也是调用9pclient，具体类似上一个方向。
 - 还可能：复用attach类型，对attach进行改造，例如Rattach的qid段改为返回节点记录的节点网络/路由表信息。
- 命名方向,问题在于动态注册时，无法知晓注册节点名称
 - 设立一个主机，由主机进行全局唯一命名。
  - 从机如何知晓主机分配的名称？这需要新协议还是复用attach？

# Kimi-2.6-pro in cc： 

## 1. 路由表：主机维护全局图，从机只做最小化转发

对于串口链路（UART/RS-485）的嵌入式拓扑，**不要**让所有节点都维护全局路由表。

### 推荐方案：分层路由

| 节点角色 | 维护的数据结构 | 职责 |
|---|---|---|
| **主机 (ESP32-P4)** | 全局邻接图 + 跳数矩阵 | 运行 BFS 计算最优路径，故障时重算 |
| **中继从机 (STM32)** | 本地端口→邻居名称表 + 下一跳缓存 | 仅转发非本机目标帧，不运行路由算法 |
| **叶子从机** | 无 | 只响应发给自己的请求 |

### 理由

- 串口拓扑通常是树状或链状，主机侧 BFS 足够高效（`CLUSTER_VFS_MAX_ROUTES=8` 的规模）。
- STM32 资源紧张，维护全局图和运行动态路由协议（如距离向量）是负担。
- 故障恢复由主机重算路径并**下发**新路由，从机被动更新即可，收敛更快也更可预测。

---

## 2. 协议改造：扩展 Mini9P 帧类型，但隔离控制层

**推荐方案：方案 2 的保守变体——新增控制帧类型，但不混入文件操作逻辑。**

复用现有的 `m9p_encode_frame` / `m9p_decode_frame` 和 CRC 机制（避免方案 1  reinvent 另一套帧格式），但在 `enum m9p_type` 中新增：

```c
TDISCOVER = 0x10,   // 拓扑发现请求
RDISCOVER = 0x90,   // 拓扑发现响应（上报自身/下游节点）
TASSIGN   = 0x11,   // 主机下发名称/路由表
RASSIGN   = 0x91,   // 确认
```

### 关键架构决策：控制面与数据面分离

在从机和主机的 UART 接收路径上，不要把这些控制消息塞进 `mini9p_server` 的文件操作回调。而是：

```
UART RX → m9p_decode_frame → 判断 type
    ├── 控制类型 (TDISCOVER/TASSIGN) → link_control_layer 处理 → 更新本地邻居表/路由缓存
    └── 9P 文件类型 (Twalk/Tread...) → mini9p_server_handle_frame → local_vfs
```

主机侧同理，新增一个 `cluster_link_manager` 模块，负责：
1. 向各 UART 端口广播/轮询 `TDISCOVER`
2. 收集 `RDISCOVER` 构建全局拓扑
3. 运行路径计算，通过 `TASSIGN` 向中继从机下发下一跳表
4. 向新节点下发分配的名称

### 为什么不选方案 1（新头部/中间层）？

- 你需要维护两套帧解析、两套 CRC、两套超时重传。对于 MCU 来说这是纯开销，收益只是"看起来更干净"。
- 复用 Mini9P 的帧头（Magic + version + CRC）不会污染语义，只要类型码分开即可。

### 为什么不选方案 3（复用 attach）？

- `Rattach` 的 `root_qid` 是文件系统的核心标识。把它改成路由信息会彻底混淆网络层和文件层语义。
- 以后该节点作为文件服务器时，`qid` 的 `type`/`version`/`object_id` 将不可信任，后患无穷。

---

## 3. 命名：主机分配，通过 TASSIGN 下发

**推荐方案：新节点初始无名称，通过发现-分配握手获得。**

### 上电/接入流程

```
新节点 (无名称)          直连邻居/主机
     │                        │
     │◄──── TDISCOVER ────────┤  (主机轮询或邻居触发)
     │                        │
     ├──── RDISCOVER ────────►│  携带硬件 ID (如 STM32 UID)
     │                        │
     │◄──── TASSIGN{name} ────┤  主机分配全局唯一名 mcuN
     │                        │
     ├──── RASSIGN ──────────►│  确认，写入本地 littleFS 配置
```

### 从机如何持久化名称？

- 收到 `TASSIGN` 后，从机将名称写入 littleFS 的一个特殊文件（如 `/sys/node_name`）。
- 下次重启直接读取该文件，无需重新握手（除非被主机强制重命名）。
- 这样 9P 的 `Tattach` 完全不需要关心节点名称——名称是链路管理层的状态。

### 为什么不在 attach 中解决命名？

- `Tattach` 是**会话建立**消息，不是**网络加入**消息。节点必须先属于网络（有名称、有路由），才能被 attach 访问文件系统。这是两个不同阶段。

---

## 总结建议

| 问题 | 推荐方案 |
|---|---|
| 路由表 | 主机维护全局邻接图 + BFS 算路；从机只存端口级下一跳表 |
| 协议改造 | 复用 Mini9P 帧格式，新增 `TDISCOVER/RDISCOVER/TASSIGN/RASSIGN` 类型，控制逻辑放在独立的 `link_control` 层，不侵入 `mini9p_server` 的文件回调 |
| 命名 | 主机通过 `TASSIGN` 分配，从机持久化到 `/sys/node_name`；9P `attach` 保持原语义 |

# GPT-5.5-Xhigh in codex:

建议走一个比较稳的中间路线：

**路由层：主机维护全局图，节点只维护局部表。**

不要让所有节点都维护全局连接网，STM32 侧内存和同步复杂度会很快膨胀。更合适的是：

- 主机维护完整拓扑图：节点、端口、边、链路状态、跳数/质量。
- 每个节点维护：
  - 自己的节点 ID / 节点名
  - 本地邻居表：哪个串口/端口连着哪个邻居
  - 可选的转发表：`目标节点 -> 下一跳端口/节点`
- 主机根据全局图算最短路径。
- 下发路径或下一跳规则给中间节点。
- 链路断开时，中间节点只上报“邻居丢失”，主机重新计算路径。

这样兼顾了第一种方案的全局可见性，也避免了第二种方案在每个节点上做完整动态路由协议的复杂度。

**9P 改造：不要复用 attach，推荐在 9P 外面加网络 envelope。**

现在代码里 UART 层已经有 `9P + length` 帧头，`m9p_uart_transport_serve_once()` 收到完整 Mini9P frame 后直接交给 `m9p_server_handle_frame()`。所以最干净的做法是在 UART transport 和 Mini9P server/client 中间加一层“网络帧”：

```text
UART bytes
  -> mesh/network envelope
      -> type = control: 注册、命名、拓扑、路由
      -> type = mini9p: 原来的 9P frame payload
          -> m9p_server_handle_frame()
```

不要改 `Rattach.qid` 来塞路由信息。`qid` 应该继续表示文件对象身份；把节点注册、路由表、拓扑这种控制面信息塞进去，短期省事，后面会很难扩展，比如链路断开通知、重命名、路由失效、节点心跳都没法自然表达。

“新增一种 9P message type”也可以，但我不太推荐作为第一版。因为节点命名、邻居发现、路由转发发生在真正访问远端 9P 文件系统之前，它更像 transport/control plane，不应该要求中间节点理解 9P 语义。

**命名：用稳定 node_id + 主机分配 name。**

每个设备启动时应该先带一个本地唯一的 `node_uid`，可以来自：

- MCU unique ID
- 烧录时写入的固定 ID
- 临时随机 ID 加冲突检测，弱一些

注册流程建议是：

```text
1. 新节点接入，发送 HELLO(node_uid, capability, local_ports)
2. 邻居节点转发 HELLO/REGISTER 到主机
3. 主机分配全局 name/node_addr
4. 主机返回 NAME_ASSIGN(node_uid, assigned_name, route_info)
5. 新节点保存当前会话名，必要时持久化
```

所以，从机知晓主机分配名称这件事，建议用新控制协议完成，不要复用 attach。后续 9P attach 只负责“挂载某个节点的文件服务”，不负责“这个节点是谁、网络怎么走”。

我会把第一版目标定成：

```text
主机：全局拓扑图 + Dijkstra/最短跳数
节点：邻居表 + 本地转发
协议：9P 外层加 mesh envelope
命名：node_uid 注册，主机分配 node_name/node_addr
```

这条路扩展性最好，也最贴合你们现在的代码结构。

# Deepseek-v4-pro in cc:

## 一、路由表层面

### 方案 A：全连接拓扑图（Link-State）

每个节点维护完整拓扑图 `G = (V, E)`，其中 V 是所有节点，E 是所有 UART 直连链路。基于此运行 Dijkstra 计算任意节点间最短路径。

**优点：**
- 主机可了解全局路径连接，可视化/调试友好
- 中间节点故障后可立即本地重算新路径，收敛时间 O(|E| + |V| log |V|)
- 支持多约束路径选择（如避开拥塞链路、按跳数/带宽选路）

**缺点：**
- 每个节点需要 O(|V|²) 内存存储拓扑（STM32F407 有 192KB SRAM，假设 32 个节点，每条链路约 20 字节，拓扑矩阵约 2KB，可行但有上限）
- 链路状态变更需要泛洪（flooding）到全网，消息复杂度 O(|V|²)
- 网络抖动时泛洪消息可能造成风暴

**适用场景：** 节点数 ≤ 32，拓扑变化不频繁，主机需要全局视图。

### 方案 B：下一跳路由表（Distance-Vector）

每个节点只维护 `target → next_hop` 映射，类似 IP 路由表。收到请求时，如果 target 不在本地直连，则转发给 next_hop，由下一跳继续解析。

当前 `cluster_vfs_route` 结构已经天然支持此模型：

```c
struct cluster_vfs_route {
    char target[CLUSTER_VFS_MAX_NAME];    // 最终目标
    char next_hop[CLUSTER_VFS_MAX_NAME];  // 下一跳
    struct m9p_client *client;            // 到下一跳的 9P 客户端
};
```

**优点：**
- 内存开销 O(|V|)，每个路由项约 40 字节，32 节点仅需约 1.3KB
- 实现简单，与现有 `cluster_vfs` 架构直接兼容
- 更新只涉及邻居交换，消息量小

**缺点：**
- 收敛慢，count-to-infinity 问题（可通过 split-horizon + poison-reverse 缓解）
- 无法做全局路径优化
- 中间节点故障需要等待距离矢量传播（秒级收敛）
- 主机无全局拓扑视图

### 推荐：混合方案

- **Master（ESP32-P4）**：维护完整链路状态拓扑图。有足够 RAM（数 MB PSRAM），可运行 Dijkstra，向运维人员暴露全局视图，故障时主动下发新路由。
- **Slave（STM32F407）**：仅维护 next_hop 转发表。收到帧时检查 dst_node，若为自己则处理，否则查表转发。
- **路由更新机制**：Master 定期泛洪链路状态；Slave 之间也可交换距离矢量作为后备（Master 不可达时降级自治）。

这类似 SDN 架构：控制面集中在 Master，数据面分布在所有节点。

### 是否所有节点都维持全局信息？

**不需要。** 理由：
1. Slave 的资源约束（192KB SRAM，其中相当部分已被 littleFS、9P server 等占用）
2. Slave 的实际职责是转发和本地文件服务，不需要知道全网拓扑
3. 如果 Master 故障，网络仍可通过距离矢量自治运转（降级模式）
4. 全局信息集中在 Master 便于管理、可视化和调试

**具体分工：**

| 功能 | Master | Slave |
|------|--------|-------|
| 路由表类型 | 链路状态图 | next_hop 转发表 |
| 路径计算 | Dijkstra | 查表转发 |
| 拓扑发现 | 主动泛洪 + 收集邻居报告 | 仅报告自己的邻居给 Master |
| 故障检测 | 心跳超时 → 重算 → 下发新路由 | 检测邻居断开 → 报告 Master |
| 数据面 | 查表转发 | 查表转发 |

---

## 二、9P 协议改造方向

### 方案 1：独立路由头 + 中间层分发（激进程度：低）

在 9P 帧外部再加一层路由头，形成两级封装：

```
┌──────────────┬────────────────────────────────┐
│ 路由头       │ 原始 Mini9P 帧                  │
│ (8-12 bytes) │ (magic + len + ... + CRC)      │
└──────────────┴────────────────────────────────┘

路由头结构：
  src_node[2]    -- 源节点 ID（2 字节，uint16）
  dst_node[2]    -- 目标节点 ID
  session_id[2]  -- 会话标识
  ttl[1]         -- 跳数限制
  flags[1]       -- 标志位（是否为路由控制帧等）
  payload_len[2] -- 内部 9P 帧长度
```

**数据流（Server 侧）：**
```
UART RX ──► 路由中间层（route_dispatch）
                │
                ├── dst == 本节点 ──► 剥离路由头 ──► m9p_server_handle_frame()
                │
                ├── dst != 本节点 ──► 查转发表 ──► 转发到 next_hop UART
                │
                └── 路由控制帧 ──► 节点注册/拓扑管理逻辑
```

**数据流（Client 侧）：**
```
UART 电平变化检测（物理连接/断开）
    │
    ▼
发送路由控制帧（邻居发现请求）
    │
    ▼
收到邻居信息 ──► 更新本地链路状态 ──► 报告 Master
```

**优点：**
- 9P 协议完全不改动，`mini9p_client` 和 `mini9p_server` 代码不加一行
- 中间转发节点不解析 9P 内容，只看路由头，转发效率高
- 路由控制（节点发现、拓扑交换）与文件操作协议彻底分离
- `design.md` 中已预留此方向（第 7 节 "routing header"）

**缺点：**
- 两套帧格式，两套 CRC，两套解析器
- Server 侧需要路由中间层先收完整帧再分流，增加一点延迟
- 路由头本身也是新的协议，需要定义、实现、测试
- Client 侧需要额外逻辑检测 UART 连接状态（物理层感知）

**实现要点：**
- 定义 `MAGIC_ROUTE`（不同于 9P 的 `0x39 0x50`），让中间层可快速区分路由帧和纯 9P 帧
- 路由帧的类型空间：`NEIGHBOR_DISCOVERY`, `NEIGHBOR_REPORT`, `LINK_STATE_UPDATE`, `NAME_ASSIGN`, `HEARTBEAT`
- 中间层在 `uart_transport` 之上、`mini9p_server` 之下

### 方案 2：扩展 9P 协议，新增消息类型（激进程度：中）

在 mini9p 协议中新增消息类型，例如：

```c
// 节点网络消息
#define M9P_TNODEINFO  0x08  // 查询/上报节点信息
#define M9P_RNODEINFO  0x88  // 返回节点信息
#define M9P_TROUTE     0x09  // 路由表交换
#define M9P_RROUTE     0x89  // 返回路由表
#define M9P_TNEIGHBOR  0x0A  // 邻居发现
#define M9P_RNEIGHBOR  0x8A  // 邻居报告
```

**数据流：**
```
UART RX ──► uart_transport ──► m9p_server_handle_frame()
                                    │
                                    ├── type ∈ {TATTACH, TWALK, ...}
                                    │      ──► 正常 9P 文件操作处理
                                    │
                                    └── type ∈ {TNODEINFO, TROUTE, ...}
                                           ──► 节点管理逻辑
```

**优点：**
- 统一协议栈，单帧格式，单 CRC 算法，单解析器
- 节点信息与文件操作使用相同的 tag 机制和错误处理（Rerror）
- 可以复用 9P 的 fid/qid 语义来表示网络拓扑（例如将路由表项当作目录项）
- Client 侧统一使用 `m9p_client` 接口发送所有消息

**缺点：**
- 9P frame type 只有 1 字节（最多 256 种类型），可用类型空间有限
- 混合了文件协议和网络控制协议，职责不够清晰
- 中间转发节点需要理解 9P 才能解析出 dst_node 做转发判断（除非把 dst 信息塞进 payload 里，但那又回到了路由头的问题）
- `m9p_server_handle_frame()` 的 switch 会越来越长

**关键问题：中间节点转发**

如果转发节点也要解析 9P 帧才能知道目标地址，那么：
- 转发节点需要维护完整的 9P server 状态机（fid 表等），即使它只是转发
- 或者：在 9P payload 里嵌入 src/dst 信息，让转发节点快速提取，这又和方案 1 趋同

这使得方案 2 在 mesh 转发场景下并不干净。

### 方案 3：复用/改造 Rattach（激进程度：高）

改造 `Rattach` 的语义，让它在建立会话的同时返回节点网络信息。

**当前 Rattach payload：**
```
negotiated_msize(2) | max_fids(1) | max_inflight(1) | feature_bits(4) | root_qid(8)
= 16 字节
```

**改造思路 A —— 在 Rattach 末尾追加节点信息：**
```
... | node_count(1) | [node_name(16) | hop_count(1)]*
```

**改造思路 B —— 重定义 root_qid 字段：**
```
root_qid.type    = 0x80 (DIR)，保持不变
root_qid.version = 本节点路由表版本号
root_qid.object_id = 本节点 ID + 邻居列表编码
```

但 `object_id` 只有 4 字节，编码不了多少信息。

**改造思路 C —— 将 Rattach 变成"节点注册 + 会话建立"二合一：**
```
Tattach: 客户端携带自身硬件 ID 和邻居信息
Rattach: 服务器返回分配的节点名 + 路由表 + 正常 9P 参数
```

**优点：**
- 最少的协议改动
- attach 本身就是"加入网络"的语义，附带节点信息是自然的
- 不需要新消息类型

**缺点：**
- **致命问题：命名时序矛盾。** Tattach 是由连接发起方（Master/上游节点）发给 Slave 的。但 Slave 的节点名需要在 Slave 响应 Rattach 之前就确定——而命名权在 Master。如果 Slave 在 Rattach 中报告自己的硬件 ID，Master 需要在 Rattach 处理完后才能分配名称，但此时会话已经建立完毕。
- Rattach 的 payload 变得很大，可能超过当前 msize 限制（512 字节）或 fid 表容量
- 路由信息随每次 attach 发送，而不随拓扑变化增量更新，浪费带宽
- qid 的语义被严重过载，从"文件标识"变成"网络标识"，后续维护困难
- 中间转发节点的 attach 和末端节点的 attach 语义不同，处理逻辑复杂

### 三种方案对比总结

| 维度 | 方案 1：路由头 | 方案 2：新 9P 类型 | 方案 3：改造 Rattach |
|------|:---:|:---:|:---:|
| 9P 改动量 | 零 | 中（新增 6-8 个类型） | 大（改变 attach 语义） |
| 中间转发效率 | 高（只看路由头） | 低（需解析 9P） | 低 |
| 职责分离 | 好 | 差（文件+网络混在一起） | 差 |
| 实现复杂度 | 中（两套解析器） | 中（一套解析器但 switch 更大） | 低起步但边界情况多 |
| 命名问题 | 有独立控制帧解决 | 有独立消息解决 | 有时序矛盾 |
| 扩展性 | 好（路由头可独立演进） | 受限于 1 字节 type | 差 |
| 与现有代码兼容 | 完全兼容 | 需要修改 m9p_server/client | 需要修改 attach 处理 |
| `design.md` 一致性 | 一致（第 7 节已预留） | 不一致 | 不一致 |

**推荐方案 1（独立路由头 + 中间层）。**

核心原因：
1. 中间转发节点不需要理解 9P——只看路由头就能转发，这是 mesh 网络的基本要求
2. 9P 完全不改动，现有测试全部保留有效
3. `design.md` 第 7 节已经为此预留了架构空间
4. 路由控制协议（节点发现、命名、拓扑交换）可以独立设计，不受 9P 语义约束

---

## 三、命名问题

### 问题描述

动态注册时，新节点通过 UART 接入已有节点。Master 负责分配全局唯一名称。但 Slave 如何知道 Master 分配了什么名字给它？

核心挑战：
- Slave 没有预先配置的名称（或只有硬件 ID，如 CPU serial number）
- 命名权在 Master（保证全局唯一）
- Slave 需要知道自己被赋予的名字才能参与路由和响应请求
- 命名发生在 9P attach 之前——因为 attach 本身需要知道"我是谁"

### 命名流程设计（基于方案 1：独立路由头）

```
        新 Slave (STM32)              邻居节点 (已有)              Master (ESP32)
             │                              │                          │
    ┌────────┴────────┐                     │                          │
    │ 上电，UART 初始化 │                     │                          │
    └────────┬────────┘                     │                          │
             │                              │                          │
             │   UART 物理连接建立            │                          │
             │  (邻居检测到 RX 线拉高/帧同步)  │                          │
             │                              │                          │
             │◄── NEIGHBOR_DISCOVERY ───────│                          │
             │    (路由控制帧: "你是谁?")      │                          │
             │                              │                          │
             │── NEIGHBOR_REPORT ──────────►│                          │
             │    (硬件ID=0x1234ABCD,        │                          │
             │     能力=9P_SERVER|STORAGE)    │                          │
             │                              │                          │
             │                              │── LINK_STATE_UPDATE ────►│
             │                              │   (新邻居通告给 Master)    │
             │                              │                          │
             │                              │◄── NAME_ASSIGN ──────────│
             │                              │   (硬件ID=0x1234ABCD     │
             │                              │    分配名称="mcu3")      │
             │                              │                          │
             │◄── NAME_ASSIGN ──────────────│                          │
             │    ("你的名字是 mcu3")         │                          │
             │                              │                          │
             │  从机保存 self.name = "mcu3"  │                          │
             │                              │                          │
             │◄── TATTACH ─────────────────│                          │
             │    (正常 9P 会话建立)          │                          │
             │                              │                          │
             │── RATTACH ─────────────────►│                          │
             │    (正常 9P 响应)             │                          │
             │                              │                          │
```

### 关键设计决策

**1. 硬件 ID 的来源：**
- STM32F407 有 96-bit unique device ID（`UID_BASE = 0x1FFF7A10`）
- 可直接用作硬件标识，无需额外配置
- 编码为 12 字节或截取低 4 字节

**2. 命名时机：**
- 命名必须在 Tattach 之前完成
- Slave 在收到 `NAME_ASSIGN` 后，将分配的名称写入 `self.name`
- 此后 Slave 可以用 `self.name` 响应路由查询和构建自己的转发表

**3. 命名策略（Master 侧）：**
- 维护 `硬件ID → 逻辑名称` 映射表
- 新节点：分配 "mcuN"（N 取最小未使用编号）
- 已见过节点：重新分配原名（基于硬件 ID 识别重连）
- 可扩展为命名规则配置（如按功能命名 "storage-1", "sensor-A"）

### 是否新协议还是复用 attach？

**必须用新协议（独立路由控制帧或新 9P 消息类型），不能单纯复用 attach。**

原因：
1. attach 是 client→server 的单向请求，且 Rattach 返回的是"会话参数"而非"节点身份"
2. Slave 需要先知道自己的名字，然后才能正确处理后续所有操作（包括被 attach）
3. 如果强行把命名塞进 Rattach：Slave 不知道自己的名字就完成了 attach——随后的路由注册、邻居通告都需要额外步骤，反而更复杂
4. 命名 + 邻居发现 + 拓扑交换 属于同一层关注点（网络控制面），应该统一处理

**如果团队强烈希望最小化新协议**，一个折中方案是：
- 保留方案 1 的路由头框架，但只定义 2 个路由控制帧类型（`NEIGHBOR_DISCOVERY` 和 `NEIGHBOR_REPORT`），在 `NEIGHBOR_REPORT` 中捎带 Master 分配的名称
- 这样只需要 1 种新的帧格式（路由帧），2 种控制类型

### 命名相关数据结构建议

```c
// 路由头中的节点 ID（逻辑名称的紧凑编码）
typedef uint16_t node_id_t;   // 支持 65535 个节点
#define NODE_ID_INVALID  0xFFFF
#define NODE_ID_MASTER   0x0001   // Master 固定为 1
#define NODE_ID_UNNAMED  0x0000   // 尚未分配名称

// 路由控制帧：邻居发现响应 / 节点注册报告
struct route_neighbor_report {
    uint8_t  hardware_id[12];   // STM32 UID (96-bit) 或用更短字段
    uint16_t capabilities;      // 能力位掩码
    uint8_t  name_len;          // 已分配名称长度（未分配则为 0）
    char     name[16];          // 已分配名称（NAME_ASSIGN 前为空）
};

// 路由控制帧：名称分配
struct route_name_assign {
    uint8_t  hardware_id[12];   // 匹配目标硬件
    uint16_t assigned_node_id;  // 分配的 node_id
    uint8_t  name_len;
    char     name[16];          // "mcu3"
};
```

### 关于"从机如何知晓主机分配的名称"的结论

**需要新协议（独立路由控制帧），但工作量小。** 最少只需定义一种路由帧格式 + 两种控制消息类型（邻居报告/名称分配）。这个新协议与 9P 完全独立，不修改 `mini9p_protocol.h` 的任何现有定义。命名流程在 attach 之前完成，时序清晰，没有语义过载。

---

## 四、推荐整体方案

| 层面 | 选择 | 要点 |
|------|------|------|
| 路由拓扑 | 混合：Master 全局图 + Slave next_hop 表 | SDN 风格，控制面集中，数据面分布 |
| 协议改造 | 方案 1：独立路由头 + 中间层 | 9P 不动，转发效率高，design.md 已预留 |
| 命名 | 路由控制帧 NAME_ASSIGN | 两阶段：发现→命名→attach |
| Slave 发现 | UART 物理连接检测 + 路由帧握手 | 邻居检测后主动发送 NEIGHBOR_DISCOVERY |

**实现优先级建议：**
1. 定义路由头格式和路由控制帧类型
2. 实现 Master 侧的 route_dispatch 中间层（帧分流）
3. 实现 Slave 侧的 route_dispatch 中间层
4. 实现邻居发现 + 命名分配流程
5. 实现 Master 全局拓扑图 + Dijkstra
6. 实现 Slave next_hop 转发表 + 转发逻辑
7. 实现链路状态泛洪和故障检测
