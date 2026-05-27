# Mesh Envelope 协议规范（v1）

## 1. 目标

本规范定义集群控制面和多跳转发层使用的 Mesh Envelope 协议。

核心边界：

- mini9P 继续只负责文件访问语义。
- Mesh Envelope 负责投递控制信息与数据面封装。
- 中继节点只解析信封头，不解析 mini9P 负载内容。

## 2. 范围与非目标

范围内：

- 线缆帧格式、完整性校验与解码约束。
- 控制面消息格式（注册、分配、探活、路由更新等）。
- 基础转发行为与重传元信息。

范围外（v1 不做）：

- 加密与签名认证。
- 分布式全网路由算法下沉到所有从机。
- 大帧分片/重组机制（超出当前最大 payload 约束）。

## 3. 线缆帧格式

帧结构：

| 字段 | 长度 | 说明 |
|---|---:|---|
| Magic | 2 字节 | 固定为 MH，用于流重同步 |
| FrameLen | 2 字节 | Version 到 Payload 末尾字节数 |
| Version | 1 字节 | 协议版本，v1 固定 0x01 |
| Type | 1 字节 | 消息类型 |
| Src | 1 字节 | 源节点短地址 |
| Dst | 1 字节 | 目标节点短地址 |
| Seq | 2 字节 | 请求序号，用于重传与去重 |
| Hop | 1 字节 | 剩余跳数（TTL 类） |
| Flags | 1 字节 | 标志位 |
| Payload | N 字节 | 控制面或数据面负载 |
| CRC16 | 2 字节 | 对 Version..Payload 计算 CRC-16/CCITT-FALSE |

长度关系：

- FrameLen = 8 + payload_len
- 整帧长度 = 2 + 2 + FrameLen + 2 = FrameLen + 6

解码校验要求：

1. Magic 必须是 MH。
2. FrameLen 必须大于等于 8。
3. 输入帧长度必须与 FrameLen 对应。
4. CRC 必须匹配。
5. Version 必须被实现支持。

## 4. 地址与身份语义

- Src/Dst 为网络运行时短地址，用于投递与转发。
- 0xFF 表示未分配地址（bootstrap 阶段）。
- UID 是设备硬件身份（稳定）。
- 节点名是主机分配的用户可见逻辑名。
- 节点短地址可在重连后改变。

## 5. Flags 定义

| 标志 | 位 | 含义 |
|---|---:|---|
| MESH_FLAG_NEEDS_ACK | bit0 | 该帧在上层语义需要确认 |
| MESH_FLAG_IS_RETRY | bit1 | 该帧是重传帧 |
| MESH_FLAG_CONTROL | bit2 | 控制面消息 |

建议：

- mini9P 数据面帧必须清除 CONTROL。
- 控制面消息应设置 CONTROL。
- 影响状态的一致性消息建议设置 NEEDS_ACK。

## 6. 消息类型

| 类型 | 值 | 平面 | 用途 |
|---|---:|---|---|
| MESH_TYPE_MINI9P | 0x01 | 数据面 | 承载完整 mini9P 帧 |
| MESH_TYPE_REGISTER | 0x10 | 控制面 | 新节点注册请求 |
| MESH_TYPE_ASSIGN | 0x11 | 控制面 | 主机分配地址与节点名 |
| MESH_TYPE_PING | 0x12 | 控制面 | 探活请求 |
| MESH_TYPE_PONG | 0x13 | 控制面 | 探活响应 |
| MESH_TYPE_TIME_SYNC | 0x14 | 控制面 | 四时间戳时钟同步 |
| MESH_TYPE_ROUTE_UPDATE | 0x15 | 控制面 | 路由项更新 |
| MESH_TYPE_LINK_STATE | 0x16 | 控制面 | 邻居链路状态上报 |
| MESH_TYPE_ERROR | 0x7F | 控制面 | 错误上报/响应 |

## 7. 控制面负载定义（v1）

所有整数均为小端序。

### 7.1 REGISTER（固定 16 字节）

| 字段 | 长度 | 说明 |
|---|---:|---|
| uid | 8 | 设备稳定身份 |
| boot_nonce | 4 | 本次上电实例标识 |
| capability_bits | 2 | 能力位图 |
| port_bitmap | 1 | 本地端口能力位图 |
| wifi_supported | 1 | 当前节点是否启用 Wi-Fi mesh 传输 |

行为建议：

- 源地址可为未分配地址。
- 目标地址使用未分配地址（按网络约定上送主机方向）。
- 设置 CONTROL 与 NEEDS_ACK。
- port_bitmap 的最高位保留给 Wi-Fi 传输；当 wifi_supported=true 时应同时置位该保留位。

### 7.2 ASSIGN（变长）

| 字段 | 长度 | 说明 |
|---|---:|---|
| uid | 8 | 目标设备 UID |
| node_addr | 1 | 分配的短地址 |
| lease_ms | 4 | 租约或有效期 |
| epoch | 2 | 分配版本/纪元 |
| name_len | 1 | 节点名长度 |
| node_name | name_len | 节点名字节串 |

校验规则：

1. name_len <= 31
2. payload_len 必须等于 16 + name_len

### 7.3 PING/PONG（固定 4 字节）

| 字段 | 长度 | 说明 |
|---|---:|---|
| local_time_ms | 4 | 本地时间戳（毫秒） |

### 7.4 TIME_SYNC（固定 16 字节）

| 字段 | 长度 | 说明 |
|---|---:|---|
| t0_master_send | 4 | 主机发送时刻 |
| t1_slave_recv | 4 | 从机接收时刻 |
| t2_slave_send | 4 | 从机发送时刻 |
| t3_master_recv | 4 | 主机接收时刻 |

常用估算公式：

- RTT 估计 = (t3 - t0) - (t2 - t1)
- 偏移估计 = ((t1 - t0) + (t2 - t3)) / 2

### 7.5 ROUTE_UPDATE（固定 6 字节）

| 字段 | 长度 | 说明 |
|---|---:|---|
| dst | 1 | 目标地址 |
| next_hop | 1 | 发送选择器；普通场景下通常是下一跳地址，子机多串口 direct-table 场景下是本地出口端口号，最高位保留选择器表示 Wi-Fi |
| metric | 1 | 路由代价 |
| route_version | 2 | 路由版本 |
| action | 1 | 1=set，2=delete |

### 7.6 LINK_STATE（固定 4 字节）

| 字段 | 长度 | 说明 |
|---|---:|---|
| neighbor | 1 | 邻居地址 |
| link_up | 1 | 0=down，非0=up |
| quality | 1 | 链路质量指标 |
| local_port | 1 | 当前 src 节点若要发往 neighbor，应从哪个本地端口发出 |

### 7.7 ERROR（固定 4 字节）

| 字段 | 长度 | 说明 |
|---|---:|---|
| code | 2 | 错误码 |
| related_seq | 2 | 关联请求序号 |

## 8. 新节点上线流程（建议）

1. 新节点上电，只有 UID，无正式短地址。
2. 节点发送 REGISTER。
3. 中继仅基于 envelope 头进行转发。
4. 主机识别 UID，分配或恢复节点名，分配短地址。
5. 主机发送 ASSIGN。
6. 节点切换为正式地址并进入在线状态。

重连建议：

- 同 UID 尽量复用节点名，保持用户路径稳定。
- 短地址允许重新分配。

## 9. 断联处理（建议）

采用两阶段：

1. SUSPECT：短时无响应，进入疑似离线。
2. OFFLINE：重试后仍超时，标记离线并撤销可达路由。

工程建议：

- 离线后不要立即删除 UID 到节点名映射。
- 对所有 in-flight 请求返回可识别网络错误。

## 10. 中继转发规则

中继处理顺序建议：

1. 校验结构与 CRC。
2. 若 Dst 为本机地址，交本地处理。
3. 若 Hop 为 0，直接丢弃。
4. 否则 Hop 减 1，根据路由表转发。
5. 不解析 mini9P payload。

## 11. 可靠性与重传建议

- Seq 用于重传匹配与重复检测。
- 重传时保持相同 Seq 和相同 payload。
- 重传帧应设置 MESH_FLAG_IS_RETRY。
- 关键控制消息建议配合 NEEDS_ACK。

## 12. 资源约束与扩展

v1 默认约束：

- MESH_MAX_PAYLOAD_LEN = 512 字节。

若后续需要更大数据：

- 可先提高上限并评估 RAM/时延开销。
- 再考虑引入独立分片机制。

## 13. 版本兼容策略

- 解析器对不支持的 Version 直接拒绝。
- 新版本应保留 Magic/FrameLen/CRC 基本语义。
- 新消息类型应定义明确的“未知类型处理策略”。

## 14. 安全说明（后续项）

v1 不含加密和认证。若用于生产环境，建议后续增加：

- 帧认证（MAC 或签名）。
- REGISTER 的接入鉴权策略。
- 按 src + seq + 时间窗的重放保护策略。
