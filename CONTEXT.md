# PWOS 领域术语

本文档只定义现行实现中的术语。历史文档使用旧名称时，以这里和源码为准。

## 身份和命名

**节点 UID**：STM32 物理设备的稳定身份，当前表示为三个 32 位值。

**boot ID**：一次设备启动的随机/变化标识。UID 相同但 boot ID 改变表示节点重启，
旧 session、fid、pending 和 job 不能继续复用。

**mesh 地址**：STM32 平面的 8 位短地址。`0x00` 是 host，`0xff` 是未分配/广播。

**节点名**：用户可见的 `mcuN`。单主机时由本机分配，多主机时由 leader 分配全局名。

**owner host**：直接管理某个 STM32 子树并能实际发出 UART 请求的 ESP32 主机。

## STM32 平面

**link frame**：`pwos-shared/link` 定义的 v2 线缆帧。旧文档中的 mesh envelope 指其
前身，不是现行模块名。

**port manager**：管理物理 UART peer、HELLO 状态和端口健康，不维护全局路由。

**node control**：管理本机地址、lease、upstream、relay pending 和路由转发。

**coordinator**：ESP32 上 STM32 平面的控制面权威，分配地址并计算路由。

**upstream**：节点到当前 coordinator authority 的首跳端口。

**route**：STM32 本地 `(dst, next_hop, port, metric, version)` 转发表项。

## 数据面

**mini9P**：远端文件协议，对用户暴露 `/mcuN/...`。

**MCU RPC**：`DATA_RPC` 承载的短调用、流式调用、通知和取消。

**Job**：`DATA_JOB` 承载的异步计算任务，可查询状态、读取结果、取消和 retry。

**typed pending**：主机按 `(data_type, src_addr, tag)` 匹配响应的并发表。

**cluster VFS**：主机侧 `/mcuN/...` 命名空间、mini9P session 和 fd 映射，不拥有
UART DMA 或 STM32 全局拓扑。

## 主机间平面

**host RPC**：ESP32 主机间的 TCP/CBOR 协议，不封装进 STM32 link frame。

**leader**：负责全局主机成员视图和 `mcuN` 命名的主机。

**follower**：接受 leader 全局命名，同时继续管理自己 STM32 子树的主机。

**host epoch**：持久化并在启动时递增的主机代次，用于选主和排除旧实例。

## 使用规则

- 用户路径使用 `mcuN`，协议和路由日志使用 mesh 地址，身份比较使用 UID。
- link 层不解析 mini9P/RPC/Job。
- STM32 relay 只依据 link 头和本地 route 转发数据帧。
- 控制面和数据面不得直接修改对方拥有的状态。
- “WiFi mesh link”是已删除的旧设计；当前 WiFi 只连接 ESP32-S3 到主机间 IP 平面。
