# 重构完成记录

状态：**M1-M10 已完成，`refactor` 已进入合并阶段。**

本文档不再作为待执行计划，而是记录本次重构解决的问题、最终模块边界和后续工作。
各阶段的详细验收见 `docs/logs/refactor/` 和对应提交历史。

## 1. 重构目标

旧系统的主要问题是：

- 裸机轮询和长关中断窗口导致 UART 丢帧。
- ISR、transport、控制面和业务逻辑边界混乱。
- 静态上下游假设无法稳定支持多端口和 relay。
- 主机单事务接收模型导致请求错配和超时放大。
- 旧 mesh runtime 同时拥有链路、路由、VFS 和会话状态。

重构后的硬规则：

1. ISR 只搬运数据和通知任务。
2. link、端口、控制面、service、compute 各自拥有状态。
3. 主机 UART RX 只有一个消费者。
4. 并发响应按 typed pending 精确匹配。
5. 热路径使用静态内存和固定上限。
6. 主机拥有全局拓扑，STM32 只保存本地转发表。

## 2. 里程碑结果

| 阶段 | 状态 | 结果 |
|---|---|---|
| M1 | 完成 | link frame v2、双 CRC、增量 parser、golden test |
| M2 | 完成 | STM32 FreeRTOS、UART DMA/IDLE、块池和任务队列 |
| M3 | 完成 | port manager、HELLO FSM、多端口隔离 |
| M4 | 完成 | coordinator、注册、lease、link state、route update |
| M5 | 完成 | session manager、mini9P、`cluster_vfs` |
| M6 | 完成 | 全局 tag、typed pending、并发 VFS 和 generation |
| M7 | 完成 | P4 LAN、WebShell、主从 `/sys` 可观测性、故障注入 |
| M8 | 完成 | MCU unary/stream/oneway/cancel/deadline RPC |
| M9 | 完成 | Job System、compute worker、结果/取消/LOST/retry |
| M10 | 完成 | host RPC、mDNS、选主、全局命名、跨主机 read/write |

M10 之后追加：

- ESP32-S3 WiFi coordinator 和 LLM 推理任务迁移。
- F429 LCD、`/display` 命名空间。
- P4 Lua 分片调度和 STM32 smallpt raytrace tile kernel。
- 三 MCU 拓扑及跨节点计算演示。

## 3. 最终代码结构

```text
pwos-shared/
  link/       线缆帧、CRC、parser
  mesh2/      控制 payload
  mini9p/     文件协议
  rpc/        MCU RPC
  job/        Job System
  host_rpc/   主机间 CBOR RPC 和选主
  render/     MCU smallpt kernel

pwos-master-esp32p4/
  coordinator_runtime/ host_coordinator/ host_sessions/
  host_api/ slave_rpc/ host_jobs/ host_rpc/
  host_shell/ host_net/ web/ lua/ render/ inference/

pwos-master-esp32s3/
  main/ host_net/ inference/

pwos-slave*/User/
  drivers/ link/ mesh2/ os/ rtos/ service/ compute/ backend/ diag/
```

## 4. 已删除的旧实现

- `pwos-shared/mesh/` legacy envelope/processor/cluster/transport。
- 旧 P4 mesh host service、VFS bridge、shell/Lua glue 和 PC master emulator。
- 裸机 slave mesh runtime 和拆分式旧 VFS backend。
- 未接入固件的 lttit `csc` 实验副本。
- 未接入当前命名空间的 littlefs/SD 自测后端和 F429 cube demo。
- 构建产物、Windows 测试可执行文件、clangd 索引和工具会话缓存。

## 5. 已验证场景

- 两节点和三节点 UART 多跳注册、租约、路由与 health。
- mini9P 与 RPC 并发，stream deadline、cancel 和 oneway。
- Job 生命周期、结果、取消、节点重启后 LOST/retry。
- P4 Ethernet WebShell 和运行时诊断。
- P4/S3 同 LAN 的 host RPC 软件构建与 PC 测试。
- 多 MCU 分片渲染并在 F429 240x320 LCD 更新。

## 6. 合并后的后续工作

这些属于产品演进，不再是本次重构阻塞项：

- 完成 P4/S3 双主机长期上板、断网切主和恢复测试。
- 为 host RPC 增加鉴权、加密和重放保护。
- 实现跨主机目录 list/stat 和 `DATA_BULK`。
- 为三角/多路径拓扑加入稳定的链路质量、去环和路由抖动抑制策略。
- 为 raytrace 增加跨帧累积和更低噪声采样。
- 明确分布式 LLM 的切分边界、张量格式和带宽预算。

当前架构和构建方式分别见：

- `docs/architecture.md`
- `docs/protocol_spec.md`
- `docs/build_and_flash.md`
