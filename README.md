# 9P-Walkers

## Logo

![team logo](docs/figs/logo.png)

## Poster

![poster](docs/figs/poster.png)

## 项目简介

本项目是中国科学技术大学 2026 春季学期 OSH-2026 课程小组 **9P Walkers** 的仓库，目标是实现一个基于 **Mini9P 协议** 的 MCU 集群分布式系统。

系统以 ESP32-P4 作为主控节点，以 STM32 系列 MCU 作为从节点。主控通过 UART/SPI/WiFi 等链路连接多个从机，并在上层提供 C Shell、Lua 运行时和 Web Shell。从机将本地文件、状态和外设抽象成文件节点，主控通过统一的集群 VFS 路径访问这些资源，例如：

```bash
cat /mcu1/sys/health
echo 100 > /mcu2/motor/speed
```

## 项目成员

- 董宇皓
- 霍斌
- 刘亦航
- 刘子源
- 武文韬

## 核心功能

- **外设即文件**：从机通过 `local_vfs` 暴露本地文件、状态和外设，Mini9P server 将文件操作转发给后端回调。
- **统一集群命名空间**：主控 `cluster_vfs` 将 `/mcuN/...` 形式的全局路径映射到对应 Mini9P client 和远端 fid。
- **Web 远程 Shell**：ESP32-P4 主控提供 HTTP/WebSocket 页面，浏览器可直接访问设备 Shell。
- **嵌入式 Lua 运行时**：主控内嵌 Lua，用于脚本化编排文件访问和设备控制。
- **Mesh/路由层**：`pwos-shared/mesh` 提供集群 envelope、节点运行时、处理器、拓扑维护和 UART/WiFi transport 组件，已支持多跳转发和主机集中式路由下发。
- **PC 联调工具**：`tools/pc_master_emulator` 可通过 USB-TTL 对从机执行 Mini9P smoke test。

## 系统架构

```text
Shell / Lua / WebShell
  -> cluster_vfs
  -> mini9p_client
  -> mesh_host_runtime
  -> mesh MINI9P envelope
  -> mesh_uart_transport
  -> mesh_node_runtime / mesh_processer
  -> mini9p_server
  -> local_vfs backend / 文件树 / 外设回调
```

### 主要目录

| 目录 | 说明 |
| --- | --- |
| `pwos-master-esp32p4/` | ESP32-P4 主控工程，包含 Shell、Lua、Web Shell、VFS bridge、transport 组装 |
| `pwos-slave/` | STM32F407 风格从机工程，包含 HAL、littleFS、local VFS、Mini9P service 集成 |
| `pwos-slave-stm32f411/` | STM32F411 从机变体，复用 `pwos-slave/User/`，只改板级 init |
| `pwos-shared/mini9p/` | 主从共享 Mini9P 协议、client、server、service 代码 |
| `pwos-shared/mesh/` | 共享 mesh envelope、cluster、processer、node_runtime、transport 代码 |
| `tools/pc_master_emulator/` | PC 端 Mini9P 主控模拟器，用于硬件串口 smoke test |
| `docs/` | 展示、报告和图片资料 |

### Mini9P 层

Mini9P 是本项目使用的轻量级 9P 变体。帧格式包括 magic `0x39 0x50`、小端长度、版本、消息类型、tag、payload 和 CRC-16/CCITT-FALSE。

共享实现位于 `pwos-shared/mini9p/`：

- `mini9p_protocol`：帧编解码、请求解析、响应构造和 CRC 校验。
- `mini9p_client`：主控侧 attach/walk/open/read/write/stat/clunk 封装。
- `mini9p_server`：从机侧 session、fid 表、权限检查和 backend 分发。

协议细节见：

- `pwos-master-esp32p4/README.md`
- `pwos-shared/mini9p/README.md`

### Mesh 层

Mesh 层负责把 Mini9P 数据面和 REGISTER/ASSIGN/ROUTE_UPDATE 等控制面统一封装成可路由的 envelope。帧格式包括 magic、长度、版本、type、src/dst、seq、hop、flags、payload 和 CRC-16/CCITT-FALSE。

共享实现位于 `pwos-shared/mesh/`：

- `envelope/mesh_protocal`：mesh 帧编解码、控制 payload 构造/解析、CRC 校验。
- `processer/mesh_processer`：按目标地址分流、转发、本机 Mini9P dispatch、控制帧回调。
- `cluster/cluster`：拓扑/路由管理。主机使用 `TOPOLOGY` 模式维护全图并派生路由；从机使用 `DIRECT_TABLE` 模式维护简化路由表。
- `transport/mesh_uart_transport`：多平台 UART 适配（ESP-IDF / STM32 HAL / POSIX），STM32 侧已实现 DMA + IDLE 中断 + frame queue。
- `pwos-master-esp32p4/mesh/mesh_wifi_link`：ESP32-P4 侧 TCP/9000 WiFi 透传链路，与 UART 同构收发 mesh 帧。

协议细节见：

- `docs/protocol_spec.md`：顶层协议栈说明（mesh envelope + mini9P payload）
- `docs/mesh_envelope_spec.md` / `pwos-shared/mesh/envelope/mesh_protocol_spec.md`：mesh envelope 详细规范
- `docs/architecture.md`：当前系统架构与目录结构
- `docs/adr/0001-mesh-envelope-for-control-plane.md`
- `docs/adr/0002-master-owns-global-topology.md`

### 主机 Runtime / Service

主机侧 runtime 负责把 cluster topology、Mini9P client、mesh processor 和底层 transport 连接起来。它维护全局视角的节点发现和路由下发流程：

- `pwos-master-esp32p4/mesh/mesh_host_runtime`：处理 REGISTER、LINK_STATE、ROUTE_UPDATE 和 Mini9P 响应分流。
- `pwos-master-esp32p4/mesh/mesh_host_service`：ESP32-P4 主控侧 service 组装层。
- `pwos-master-esp32p4/cluster/cluster_config`：主机集群配置、节点发现回调和命名绑定。

运行链路说明见：

- `pwos-shared/RUN_TIME_NODE_MESH.md`
- `pwos-master-esp32p4/README.md`

### 从机 Runtime / Service

从机侧 runtime 负责节点启动注册、地址分配后的本机地址维护、邻居探测、Mini9P server dispatch 和多 UART 端口转发。

主要实现位于 `pwos-slave/User/mesh/`：

- `mesh_node_runtime`：REGISTER/ASSIGN、邻居探测、路由表、Mini9P 收发分流。
- `mesh_node_service`：板级 service 组装层，管理多个 UART mesh 端口和 `addr -> port` 学习表。
- `mesh_node_mini9p_init`：把 node VFS、Mini9P server、mesh service 和板级 UART handle 连接起来。

F411 变体复用大部分从机业务代码，只在 `pwos-slave-stm32f411/User/app/mesh_node_mini9p_init.c` 中适配 UART handle 和端口配置。

### 从机 VFS 后端

从机把本地资源抽象为 Mini9P 文件节点，Mini9P server 只负责 session/fid/权限管理，实际资源访问由 VFS 后端完成。

主要实现位于 `pwos-slave/User/backend/`：

- `node_vfs`：节点级路由入口，组合 `/sys`、`/dev`、`/fs`。
- `sys_vfs`：虚拟系统节点，如 `/sys/health`、`/sys/info`、`/sys/routes`、`/sys/log`。
- `dev_vfs`：设备节点后端。
- `lfs_vfs`：littlefs 文件系统后端。
- `local_vfs`：早期本地 VFS 后端，保留给测试和兼容路径。

后端测试位于：

- `pwos-slave/User/backend/test/`
- `pwos-slave-stm32f411/User/backend/test/`

### Cluster VFS

`pwos-master-esp32p4/vfs_bridge/cluster_host_vfs.c` 是主控上的统一命名空间桥接层。它不是完整 POSIX 文件系统，而是维护 `UID <-> 节点名 <-> mini9P client` 的绑定表和本地 fd 表：

- 路径解析：`/mcu1/dev/temp` 命中 `mcu1`，发送到从机时转为 `/dev/temp`。
- 本地 fd：`local_fd -> route + remote_fid`，其中 `remote_fid` 是 Mini9P session 内的远端句柄。
- 节点发现：由 `mesh_host_runtime` + `cluster_config` 自动驱动，按 UID 复用旧名字。
- 可达性：按共享 `mesh cluster` 的拓扑变化自动刷新，不可达节点回退到未 attach 状态，但保留 UID↔名字映射。
- 当前已实现 direct route、多跳中继路由、attach/detach、open/read/write/stat/close、路径级 read/write 和目录 list。
- 自动重连、路径规范化、并发请求仍是扩展方向。

详细设计见 `pwos-master-esp32p4/vfs_bridge/design.md`。

### PC 主控模拟器

PC emulator 是硬件 smoke test 和串口联调工具，用 PC 串口模拟主控侧 mesh host runtime。

实现位于 `tools/pc_master_emulator/`：

- 打开 USB-UART，接收从机 bootstrap `REGISTER`。
- 分配 `mcuN` 名称和 mesh 地址，发送 `ASSIGN`。
- 等待 `LINK_STATE`，触发 host runtime 下发 `ROUTE_UPDATE`。
- 对 `/mcuN/sys/health`、`/mcuN/sys/routes`、`/mcuN/sys/log` 执行 Mini9P 读操作。
- 实时打印 Mesh/Mini9P 收发日志，并在退出前尽量读取节点 `/sys/log`。

工具说明见：

- `tools/pc_master_emulator/README.md`

## 构建与运行

### ESP32-P4 主控（暂未验证）

需要先导出 ESP-IDF 环境，使 `IDF_PATH` 可用。

```bash
cd pwos-master-esp32p4
idf.py build
idf.py flash
idf.py monitor
# 或一条命令完成
idf.py build flash monitor
```

主控 `main/CMakeLists.txt` 会嵌入 `web/index.html`，并编译 `pwos-shared/mini9p` 与 `pwos-shared/mesh` 中的共享代码。

ESP-IDF pytest：

```bash
cd pwos-master-esp32p4
pytest pytest_hello_world.py
```

### STM32 从机

STM32F407 模板：

```bash
cd pwos-slave

# 普通 Debug
cmake --preset Debug
cmake --build --preset Debug

# 启用 Mini9P 串口联调
cmake --preset F407Debug
cmake --build --preset F407Debug

# Release
cmake --preset Release
cmake --build --preset Release
```

`F407Debug` preset 启用 `PWOS_ENABLE_MINI9P_SERIAL`。旧版 `ZGT6Debug` preset 已不存在；需要 ZGT6 专用配置时可手动传入：

```bash
cmake --preset F407Debug -DPWOS_BOARD_ZGT6=ON -DPWOS_SKIP_LFS_MOUNT=ON
```

Mini9P 串口联调口通常固定在 `USART2`：`PA2=TX`、`PA3=RX`、`1000000` baud；该 USART 只传 Mini9P/mesh 二进制帧，不应混入 VOFA 或文本日志。

STM32F411 从机变体：

```bash
cd pwos-slave-stm32f411
cmake --preset Debug
cmake --build --preset Debug
cmake --preset Release
cmake --build --preset Release
```

`pwos-slave-stm32f411/` 只提供 `Debug/Release` presets。F411 当前板级配置中 `USART2`（`PA2=TX`、`PA3=RX`）是与 PC/host 通信的 mesh 主口，默认 `1000000` baud；该口只传 Mini9P/mesh 二进制帧，不应混入 VOFA 或文本日志。

## 测试

### Cluster VFS host test

```bash
cmake -S pwos-master-esp32p4/vfs_bridge/test -B pwos-master-esp32p4/vfs_bridge/test/build
cmake --build pwos-master-esp32p4/vfs_bridge/test/build
pwos-master-esp32p4/vfs_bridge/test/build/cluster_vfs_test
```

### 从机 local VFS backend test

```bash
cmake -S pwos-slave/User/backend/test -B pwos-slave/User/backend/test/build
cmake --build pwos-slave/User/backend/test/build
pwos-slave/User/backend/test/build/local_vfs_test
```

### 从机 Mini9P server 与 UART transport host tests

```bash
cmake -S pwos-slave/User/uart_transport/test -B pwos-slave/User/uart_transport/test/build
cmake --build pwos-slave/User/uart_transport/test/build
pwos-slave/User/uart_transport/test/build/mini9p_server_test
pwos-slave/User/uart_transport/test/build/slave_uart_transport_host_test
```

### STM32F411 Mini9P server test

```bash
cmake -S pwos-slave-stm32f411/User/backend/test -B pwos-slave-stm32f411/User/backend/test/build
cmake --build pwos-slave-stm32f411/User/backend/test/build
pwos-slave-stm32f411/User/backend/test/build/local_vfs_test
```

### PC 主控模拟器硬件 smoke test

```bash
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000 1
```

该工具会依次执行 `Tattach`、`Twalk /sys/health`、`Topen`、`Tread`、`Tclunk`，并检查 `/missing` 和 bad fid 的错误路径。成功时输出：

```text
pc_master_emulator: ok
```

连接约定见 `tools/pc_master_emulator/README.md`。

## 开发约定

- Mini9P 协议、client、server、peer_link 的共享实现优先修改 `pwos-shared/mini9p/`。
- 从机侧热路径尽量保持固定 buffer、静态表和可预测执行时间。
- 主控上层入口应优先通过 `cluster_vfs` 访问远端资源，不直接耦合底层 UART/SPI/WiFi transport。
- 外设控制尽量保持“文件节点 + read/write 回调”的模型。

## 项目进展

| 阶段 | 日期 | 进展 |
| --- | --- | --- |
| 前期调研 | 3/9~3/15 | 调研 MCU 集群、分布式文件系统、轻量 RTOS、脚本运行时等方向 |
| 集中调研 | 3/16~3/20 | 深入调研各模块可行性 |
| 会议立项 | 3/21 | 确定选题为 MCU 集群项目 |
| 方案讨论 | 4/2 | 确定主从架构、Mini9P、Shell、Lua、Web Shell、VFS 等模块分工 |
| 早期开发 | 4/3~4/15 | 搭建主控、从机、协议和 VFS 的初步系统框架 |
| 中期推进 | 4/16 之后 | 补充共享 Mini9P、cluster VFS、mesh、host test 和 PC 串口联调工具 |
| mesh 路由落地 | 5月 | 实现 mesh envelope 控制面、多跳转发、主机拓扑派生路由、ROUTE_UPDATE 下发 |
| DMA + IDLE transport | 5月 | STM32 UART transport 改为 DMA + IDLE 中断 + frame queue，解决多 UART 串联阻塞 |
| WiFi 链路 | 5月 | ESP32-P4 增加 `mesh_wifi_link` TCP/9000 透传接入 |
| 文档同步 | 6月 | 重写 architecture/protocol_spec/vfs_bridge/design 等文档，补齐 mesh 相关说明 |

## 相关文档

| 文档 | 内容 |
| --- | --- |
| `docs/architecture.md` | 当前系统架构与目录结构 |
| `docs/protocol_spec.md` | 顶层协议栈（mesh envelope + mini9P payload） |
| `docs/mesh_envelope_spec.md` | mesh envelope 协议规范 |
| `docs/mesh_wifi_link.md` | ESP32-P4 WiFi 透传链路 |
| `docs/slave_mesh_runtime.md` | 从机 runtime 开发者指南 |
| `docs/cluster_config_usage.md` | `cluster_config` 与 VFS 协作流程 |
| `docs/transport_abstraction.md` | UART/WiFi transport 抽象与 STM32 DMA 设计 |
| `docs/build_and_flash.md` | 编译烧录指南 |
| `pwos-shared/RUN_TIME_NODE_MESH.md` | 主机侧 runtime 调用指南 |
| `tools/pc_master_emulator/README.md` | PC 串口联调指南 |
