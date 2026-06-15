# mesh_wifi_link：ESP32-P4 主机侧 WiFi/局域网 mesh 链路

## 1. 功能定位

`mesh_wifi_link` 是 ESP32-P4 主机侧的**局域网 TCP 透传链路**：

- ESP32-P4 本身不带 WiFi 射频，无线一跳由局域网路由器或从机侧 WiFi 透传模块完成。
- 本模块在以太网上监听一个 TCP 端口（默认 `9000`），从机侧 WiFi 透传模块（ESP8266/ESP32 AT 透传，或任何 TCP client）连入后，原始 mesh 帧以与 UART 完全同构的字节流在该连接上透传。
- 帧边界靠 mesh envelope 自描述（`'M''H'` + `FrameLen`），TCP 仅提供可靠字节流。

源码位置：

- `pwos-master-esp32p4/mesh/mesh_wifi_link.c`
- `pwos-master-esp32p4/mesh/mesh_wifi_link.h`

## 2. 核心状态机

```
[mesh_wifi_link_start(port)] -> listening on TCP/port
       │
       ▼
[accept one client] -> non-blocking TCP socket
       │
       ├── recv() 驱动帧重组缓冲
       │        └── 提取完整 mesh 帧 -> 学习 src -> 返回给调用方
       │
       └── send() 向当前 client 写完整 mesh 帧
```

关键状态保存在 `struct mesh_wifi_link_state`（`mesh_wifi_link.c:25`）：

| 字段 | 说明 |
|------|------|
| `active` | 是否处于监听状态。 |
| `tcp_port` | 当前监听端口。 |
| `listen_fd` / `client_fd` | 监听 socket 与当前已连接 client。 |
| `rx_buf` / `rx_have` | TCP 字节流重组缓冲，容量 1024 字节。 |
| `addrs` / `addr_count` | 经本链路收到过的 src 地址表，最多 8 个。 |

## 3. 与 mesh_host_service / mesh_host_runtime 的协作

`mesh_host_service` 在 `mesh_host_service_receive_frame()`（`mesh_host_service.c:323`）中按以下顺序轮询链路：

1. 先调用 `mesh_wifi_link_receive_frame()`；收到帧后将 `out_ingress_port` 置为 `MESH_HOST_SERVICE_WIFI_INGRESS_PORT`（`0x80`）。
2. 无帧时再扫描 UART 端口。

发送路径在 `mesh_host_service_send_frame()`（`mesh_host_service.c:280`）：

- 若 `mesh_wifi_link_owns_addr(next_hop)` 为真，直接走 TCP 单播。
- 若 `next_hop == MESH_ADDR_UNASSIGNED`（bootstrap 帧，如 `ASSIGN dst=0xFF`），同时向 TCP 链路与 UART 链路广播，保证 WiFi 侧未分配节点也能收到地址分配。
- 其他情况仍走 UART 端口。

`mesh_host_runtime` 通过 `mesh_host_service` 提供的 `send_frame/receive_frame` 回调与 WiFi 链路间接交互，无需直接引用 `mesh_wifi_link`。

## 4. 端口选择器与 WiFi 保留位

mesh 协议在 `pwos-shared/mesh/envelope/mesh_protocal.h:40` 中定义：

```c
#define MESH_PORT_SELECTOR_WIFI_ID   7u
#define MESH_PORT_SELECTOR_WIFI_MASK ((uint8_t)(1u << MESH_PORT_SELECTOR_WIFI_ID))
```

- `REGISTER.port_bitmap` 的最高位（bit 7）预留给 Wi-Fi 传输，与 `CLUSTER_PORT_WIFI_ID`（`pwos-shared/mesh/cluster/cluster.h:40`）对齐。
- 当前 `mesh_wifi_link` 本身不解析 `port_bitmap`；它只负责“单条 TCP 链路收发原始帧”。
- `MESH_HOST_SERVICE_WIFI_INGRESS_PORT`（`0x80`）是主机 service 给 WiFi 入口分配的虚拟端口号，不占用 UART 槽位，也区别于 `MESH_PROCESSER_INGRESS_PORT_NONE`（`0xFF`）。

## 5. API 概览

| 接口 | 说明 |
|------|------|
| `mesh_wifi_link_start(uint16_t tcp_port)` | 启动 TCP 监听；`tcp_port=0` 时使用默认 `9000`。同一端口幂等，不同端口会重启监听。 |
| `mesh_wifi_link_stop(void)` | 停止监听并断开 client。 |
| `mesh_wifi_link_active(void)` | 是否处于监听状态。 |
| `mesh_wifi_link_owns_addr(uint8_t mesh_addr)` | 该地址是否已学习为经本链路可达。 |
| `mesh_wifi_link_send_frame(...)` | 向当前 client 发送一帧；无 client 时返回 `-MESH_ERR_NO_ROUTE`。 |
| `mesh_wifi_link_receive_frame(...)` | 非阻塞轮询：驱动 accept/recv 并从重组缓冲中提取一帧。 |
| `mesh_wifi_link_format_status(...)` | 输出监听端口、client IP、已学地址，供 shell 诊断使用。 |

## 6. 构建与使用

- 本模块只在 `ESP_PLATFORM` 下编译；PC 测试环境提供空实现（恒返回不可用）。
- 默认不需要显式调用；`mesh_host_service` 在 `mesh_host_service_start_default_task()` 中不自动启动 WiFi 链路。
- 需要 WiFi 接入时，由上层 shell/web 调用 `mesh_wifi_link_start(0)`，或启动时显式开启。

典型启动流程：

```c
mesh_wifi_link_start(0);          // 监听 TCP/9000
mesh_host_service_start_default_task();
```

## 7. 当前限制

1. **单 client**：当前只支持一个已连接 TCP client；新连接到来时会替换旧连接。
2. **单链路入口**：一个 TCP 入口后面可以挂 relay 从机带多个下游节点，因此地址表允许同一链路学习多个 src 地址。
3. **不主动发现 WiFi 节点**：依赖从机侧 WiFi 透传模块主动连入；主机不扫描、不维持 TCP client。
4. **无链路保活**：当前未实现 TCP keepalive 或应用层心跳；断链靠下一次 `send/recv` 失败检测。
5. **ESP32-P4 独占**：空实现保证 PC 测试可链接，但功能只在 ESP32-P4 上可用。

## 8. 相关文件

- `pwos-master-esp32p4/mesh/mesh_host_service.c`：接收顺序、发送广播逻辑。
- `pwos-shared/mesh/envelope/mesh_protocal.h`：`MESH_PORT_SELECTOR_WIFI_ID` 定义。
- `pwos-shared/mesh/cluster/cluster.h`：`CLUSTER_PORT_WIFI_ID` 定义。
