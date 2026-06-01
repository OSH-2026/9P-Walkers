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
- **Mesh/路由层**：`pwos-shared/mesh` 提供集群 envelope、节点运行时、处理器和 UART transport 组件，为后续多跳转发和去中心化路由预留基础。
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
| `pwos-slave-stm32f411/` | STM32F411 从机变体，包含 Mini9P 串口联调 preset |
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
- `mesh_node_service`：从机上板联调组装层，连接 `local_vfs`、server、`mesh_node_runtime` 和 `mesh_uart_transport`。

协议细节见：

- `pwos-master-esp32p4/README.md`
- `pwos-shared/mini9p/README.md`

### Cluster VFS

`pwos-master-esp32p4/vfs_bridge/cluster_host_vfs.c` 是主控上的统一命名空间桥接层。它不是完整 POSIX 文件系统，而是维护静态路由表和本地 fd 表：

- 直连路由：`/mcu1/dev/temp` 命中 `mcu1`，发送到从机时转为 `/dev/temp`。
- 本地 fd：`local_fd -> route + remote_fid`，其中 `remote_fid` 是 Mini9P session 内的远端句柄。
- 当前已实现 direct route、attach/detach、open/read/write/stat/close、路径级 read/write 和目录 list。
- 中继路由、多跳转发、动态路由、自动重连仍是扩展方向。

详细设计见 `pwos-master-esp32p4/vfs_bridge/design.md`。

## 构建与运行

### ESP32-P4 主控

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

```bash
cd pwos-slave
cmake --preset Debug
cmake --build --preset Debug
cmake --build --preset Release
```

### STM32F411 从机变体

```bash
cd pwos-slave-stm32f411
cmake --preset Debug
cmake --build --preset Debug
cmake --preset ZGT6Debug
cmake --build --preset ZGT6Debug
```

`ZGT6Debug` 会启用 `PWOS_BOARD_ZGT6` 和 `PWOS_ENABLE_MINI9P_SERIAL`。Mini9P 串口联调模式下，对应 USART 应只传输 Mini9P 二进制帧，不应混入 VOFA 或文本日志。

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
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 115200
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
