# 增加 TCP/9000 WiFi 透传链路

## 状态

accepted

## 上下文

ESP32-P4 主控板不带 WiFi 射频，但集群中的某些从机可能通过 WiFi 模块（ESP8266/ESP32 等 AT 透传模块）接入局域网。需要一种机制让这些 WiFi 节点也能像 UART 节点一样收发 mesh 帧，而不改变上层 mesh runtime 的语义。

## 决策

在 ESP32-P4 主机侧增加 `mesh_wifi_link`：

- 主机监听固定 TCP 端口（默认 `9000`）。
- WiFi 透传模块作为 TCP client 连入后，原始 mesh 帧以字节流形式在该 TCP 连接上透传。
- 帧边界仍由 mesh envelope 的 `Magic + FrameLen` 自描述。
- `mesh_host_service` 把 WiFi 链路与 UART 链路并列处理：接收时先扫描 WiFi，发送时对已学习地址走 WiFi 单播，对 bootstrap 帧（`dst=0xFF`）同时向 WiFi 和 UART 广播。

## 理由

1. **与 UART 同构**：上层 runtime 看不到传输介质差异，只需处理“原始 mesh 帧”。
2. **复用现有 envelope**：不需要为 WiFi 单独设计帧格式。
3. **最小化硬件改动**：ESP32-P4 通过以太网接入局域网，WiFi 由外部模块完成。
4. **bootstrap 兼容性**：对未分配地址的节点，ASSIGN 必须同时走 WiFi 和 UART 广播，否则 WiFi 侧节点收不到地址分配。

## 共存策略

| 场景 | 发送路径 |
|------|----------|
| `next_hop` 已学习为 WiFi 可达 | 仅走 `mesh_wifi_link_send_frame()` |
| `next_hop == MESH_ADDR_UNASSIGNED`（bootstrap） | WiFi + UART 同时广播 |
| 其他情况 | 走 UART |

接收路径：`mesh_host_service_receive_frame()` 先调用 `mesh_wifi_link_receive_frame()`，命中时返回虚拟入口端口 `MESH_HOST_SERVICE_WIFI_INGRESS_PORT`（`0x80`）。

## 当前限制

- 只支持一个 TCP client；新连接替换旧连接。
- 一条 TCP 链路可承载多个下游节点（relay），通过 src 地址学习表区分。
- 当前不实现 TCP keepalive，断链依赖 `send/recv` 失败检测。
- ESP32-P4 独占；PC 测试环境为空实现。

## 影响

- `pwos-master-esp32p4/mesh/` 增加 `mesh_wifi_link.c/.h`。
- `mesh_host_service.c` 的收发路径增加 WiFi 分支。
- mesh 协议保留 `MESH_PORT_SELECTOR_WIFI_ID`（bit 7）供未来声明 WiFi 能力。

## 相关文件

- `pwos-master-esp32p4/mesh/mesh_wifi_link.c/.h`
- `pwos-master-esp32p4/mesh/mesh_host_service.c`
- `pwos-shared/mesh/envelope/mesh_protocal.h`
- `docs/mesh_wifi_link.md`
