# TODO

## 已完成

### STM32 Mesh UART DMA + 事件队列化

已在 `pwos-shared/mesh/transport/mesh_uart_transport.c` 的 STM32 HAL 分支实现：

- 使用 `HAL_UARTEx_ReceiveToIdle_DMA` 接收字节流。
- IDLE / half / complete 中断只做事件登记和写指针记录。
- per-port frame queue 缓存完整 raw mesh frame。
- `mesh_node_service_poll_once()` / 主循环继续负责 mesh 和 Mini9P 业务，ISR 不解析帧、不调用业务函数。
- `mesh_uart_transport_get_stats()` 提供 RX overflow、bad frame 等诊断统计。

验收状态：

- [x] 单板 `PC <-> F407`：REGISTER、ASSIGN、NEIGHBOR_PROBE、LINK_STATE、ROUTE_UPDATE、attach、读取 `/sys/health` 成功。
- [x] 串联 `PC <-> A <-> B`：B 能注册为 `mcu2`，ASSIGN 能回转，A/B 都能上报 LINK_STATE，主机能读 `/mcu2/sys/health` 和 `/mcu2/sys/log`。

## 待办

### 1. Host Runtime 并发请求模型

- 当前 `mesh_host_runtime_client_request()` 仍是单事务模型：通过 `dispatch_busy` 独占接收链路，同一时刻只允许一个同步 Mini9P 请求。
- 后续可扩展为真正的 pending 表，支持多个 in-flight 请求。
- 需要处理 tag 匹配、超时、乱序响应、重传去重。

### 2. 主机侧本地 server handler

- 当前主机侧尚未为“远端主动发到主机的 mini9P T*”挂接完整本地 server handler。
- 未来需要让从机也能通过 `/host/...` 或类似路径访问主机的本地资源。

### 3. 邻居传播路由模型（评估中）

- 当前路由策略保持“控制器下发指定路由”：主机/控制器计算 `dst -> next_hop -> metric`，节点只应用收到的路由更新。
- 后续可评估邻居传播模型：每个节点向直连邻居广播可达表，接收方用“邻居宣告 metric + 1”与本地路由表比较。
- 需要注意：
  - 接收方的 `next_hop` 应该是宣告该路由的邻居地址（入站控制帧的 `src`），不应直接信任 payload 里的 `next_hop`。
  - `addr -> port` 映射优先从直连邻居控制帧学习，不能简单用普通转发数据帧的原始 `src` 推断。
  - 需要处理 count-to-infinity/环路收敛：最大 metric、路由老化、版本/序列号、split horizon 或 poison reverse。
- 如果实现，建议新增“route advertise”语义，避免和现有“控制器指定 route update”混用。

### 4. 拓扑有向/双向语义最终确定

- 当前收到 `A → B` 不自动补 `B → A`；反向路径必须由 B 自行上报 LINK_STATE。
- 考虑将来：主机直接把这样的链路视做双向连通，简化从机实现。
- 需要评估：在 Wi-Fi / 非对称链路场景下，双向假设是否总是成立。

### 5. VFS 增强

- 实现 `cluster_vfs_list_routes()` 或类似诊断接口。
- 路径规范化（防止 `//`、`..` 等异常路径）。
- 自动重连策略：节点离线后，在保留 UID↔名字映射的前提下，如何优雅地重新 attach。
- Shell 命令接入：在 C Shell / Web Shell 中展示节点列表、可达性、路由表。

### 6. WiFi Link 稳定性测试

- `mesh_wifi_link.c` 已实现 TCP/9000 透传接入。
- 需要补充：多客户端并发、断链重连、与 UART 端口混合组网的压力测试。

### 7. 多跳压力测试

- 长链路（PC <-> A <-> B <-> C）下的 REGISTER/ASSIGN/ROUTE_UPDATE 收敛时间。
- back-to-back Mini9P Tread/Rread 不截断、不串帧。
- 人为注入坏 magic/半帧后，transport 能恢复到下一帧，并在 `/sys/log` 记录错误计数。

### 8. 文档补全

- [ ] `docs/mesh_wifi_link.md`：WiFi 链路使用与配网说明。
- [ ] `docs/slave_mesh_runtime.md`：从机 runtime 开发者指南。
- [ ] `docs/cluster_config_usage.md`：`cluster_config` 与 VFS 的协作流程。
- [ ] `docs/build_and_flash.md`：ESP32-P4 / F407 编译烧录指南。
- [ ] `docs/transport_abstraction.md`：UART DMA/IDLE 设计与 WiFi 统一抽象。
- [ ] `docs/adr/0003-*.md`：补充 mesh envelope、relay bootstrap、WiFi link 的架构决策记录。
