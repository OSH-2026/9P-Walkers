# Mini9P Server 实施计划

## 目标

完成 STM32 侧 mini9P server，跑通第一条主从文件访问闭环：

```text
ESP32 cluster_vfs
  -> mini9p_client
  -> UART request
  -> STM32 mini9p_server
  -> 本地 VFS/backend
  -> STM32 mini9p_server
  -> UART response
  -> mini9p_client
  -> ESP32 cluster_vfs
```

设计边界：

```text
mini9p_protocol  只负责帧编解码、T* 解析、R* 构造
mini9p_server    负责会话状态、fid 生命周期、请求分发、Rerror
backend/本地 VFS 负责真正的 stat/open/read/write/clunk
uart_transport   负责收发完整 mini9p frame
```

## 当前状态

已完成：

```text
[x] 协议源文件改名为 mini9p_protocol.c
[x] CMake 已加入 mini9p_protocol.c / mini9p_server.c / uart_transport.c
[x] 协议层已补齐 server 方向能力
[x] mini9p_server 核心状态机已实现
[x] server 资源访问已抽象为 m9p_server_ops
[x] PC testbench 已建立
[x] fake backend 已覆盖 attach/walk/open/read/clunk 与常见错误路径
[x] local VFS v1 已接入虚拟节点 `/`、`/sys`、`/sys/health`
[x] mini9p_service 已组装 local_vfs + mini9p_server + uart_transport
[x] PC Master Emulator 串口联调工具已建立
```

仍未完成：

```text
[ ] 还没有上板验证 master <-> slave UART 闭环
[ ] master 侧 cluster_vfs/cluster_config 目前也尚未接入 app_main 启动链路
```

## 1. 协议层

状态：已完成。

server 侧需要的请求解析已经补齐：

```text
m9p_parse_tattach
m9p_parse_twalk
m9p_parse_topen
m9p_parse_tread
m9p_parse_twrite
m9p_parse_tstat
m9p_parse_tclunk
```

server 侧需要的响应构造已经补齐：

```text
m9p_build_rattach
m9p_build_rwalk
m9p_build_ropen
m9p_build_rread
m9p_build_rwrite
m9p_build_rstat
m9p_build_rclunk
m9p_build_rerror
```

协议定义仍保持 client/server 共用，不拆成两套不兼容的协议文件。

## 2. Server 核心

状态：已完成第一版。

`mini9p_server.h/.c` 已包含轻量 server 状态：

```text
attached 标志
fid 表
fid -> path/qid/open/mode/iounit 状态
协商后的 msize
根目录 qid
后端 ops 指针
read 暂存 buffer
```

对外入口：

```c
int m9p_server_handle_frame(void *server_ctx,
                            const uint8_t *request_data,
                            size_t request_len,
                            uint8_t *response_data,
                            size_t response_cap,
                            size_t *response_len);
```

handler 流程：

```text
解码 frame
检查 version / msize
按 T* type 分发
校验 fid/path/mode/open 状态
调用本地 backend ops
构造 R* 或 Rerror
```

已实现请求集合：

```text
Tattach -> Rattach
Twalk   -> Rwalk
Topen   -> Ropen
Tread   -> Rread
Twrite  -> Rwrite
Tstat   -> Rstat
Tclunk  -> Rclunk
错误    -> Rerror
```

关键约束：

```text
payload 字段顺序与 master mini9p_client 对齐
响应 tag 原样返回
R* type 与请求匹配
read/write 只能作用于已 open fid
walk 不能作用于已 open fid
不存在 fid 返回 EFID
非法路径返回 EINVAL/ENOENT
后端错误统一转换为 Rerror
```

## 3. PC Testbench

状态：已完成第一版。

目录：

```text
pwos-slave/User/mini9p/test/
```

测试链路：

```text
test_main
  -> m9p_build_t*
  -> m9p_server_handle_frame
  -> fake backend
  -> m9p_parse_r*
```

当前覆盖：

```text
Tattach -> Rattach
Twalk /sys/health -> Rwalk
Topen -> Ropen
Tread -> Rread
Tclunk -> Rclunk
非法路径 -> Rerror ENOENT
非法 fid -> Rerror EFID
```

运行方式：

```bash
cd pwos-slave/User/mini9p/test
cmake -S . -B build
cmake --build build
./build/mini9p_server_test
```

期望输出：

```text
mini9p_server_test: ok
```

## 4. Backend / 本地 VFS

状态：已开始。

不要让 `mini9p_server` 直接绑定 littlefs。推荐先做一个从机本地资源层：

```text
mini9p_server
  -> m9p_server_ops
  -> slave local_vfs/backend
      -> littlefs 普通文件
      -> 设备节点
      -> 状态节点
      -> 计算任务节点
```

第一版 backend 可以先只做只读虚拟节点：

```text
/
/sys
/sys/health
```

建议节点行为：

```text
/             目录，可读
/sys          目录，可读
/sys/health   文件，只读，返回 "ok\n" 或系统状态
```

等本地虚拟节点闭环稳定后，再把 littlefs 作为 provider 接入。

当前已新增 `User/backend/` 目录，local VFS v1 只保留本地虚拟节点：

```text
User/backend/local_vfs.h   public API + local VFS 状态结构
User/backend/local_vfs.c   m9p_server_ops glue + 虚拟节点 + dirent 编码
User/backend/test/         backend 独立 PC testbench
```

`local_vfs` 接口边界如下：

```text
m9p_server
  -> m9p_server_ops
  -> local_vfs
      -> virtual nodes (/sys, /sys/health)
```

local VFS v1 行为：

```text
stat(path)      -> 查询固定虚拟节点
open(path,mode) -> 只支持 OREAD
read(path,off)  -> 虚拟文件直接切片；目录即时编码 dirent
write(path,off) -> 返回 ENOTSUP
clunk(path)     -> 无资源释放，参数合法则返回 0
```

第一版仍保持 server 的 path-based ops。由于 local VFS v1 只有虚拟节点，不保存真实文件句柄，也不需要 open table。后续接入 littlefs、设备节点或计算节点时，再评估是否把 ops 升级为 handle/cookie-based：

```c
open(ctx, path, mode, out_handle, out_qid, out_iounit)
read(ctx, handle, offset, ...)
write(ctx, handle, offset, ...)
clunk(ctx, handle)
```

但当前 stop-and-wait UART 闭环阶段，path-based backend 足够小，风险也更低。

## 5. UART 集成

状态：已完成第一版，等待上板验证。

从机侧新增 `User/mini9p/mini9p_service.h/.c`，作为上板联调的组装层：

```text
mini9p_service_init
  -> local_vfs_init
  -> m9p_server_init(local_vfs_ops)
  -> m9p_uart_transport_init_default

mini9p_service_poll_once
  -> m9p_uart_transport_serve_once
  -> m9p_server_handle_frame
  -> local_vfs
```

`pwos-slave/CMakeLists.txt` 提供编译选项：

```bash
cmake -S pwos-slave -B pwos-slave/build/Mini9PSerial \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPWOS_ENABLE_MINI9P_SERIAL=ON
cmake --build pwos-slave/build/Mini9PSerial
```

开启 `PWOS_ENABLE_MINI9P_SERIAL` 后，`Core/Src/main.c` 的启动链路切换为 Mini9P 串口模式：

```text
MX_GPIO_Init
MX_USART2_UART_Init
mini9p_service_init
while (1) mini9p_service_poll_once
```

该模式下 USART2 只承载 Mini9P 二进制帧，不输出 VOFA 文本、fs report 或 Error_Handler 文本，避免污染协议帧流。宏未开启时保持原来的 VOFA/fs_selftest 行为。

## 6. PC Master Emulator 串口联调

状态：PC 工具已建立，推荐作为上板第一阶段。

在接 ESP32 master 之前，先用 PC 通过 USB-TTL 直接模拟 master，单独验证 STM32 侧 `uart_transport + mini9p_server + backend`。

技术路线：

```text
PC (master emulator)                    STM32F407 (slave)
┌───────────────────┐    USB-TTL       ┌────────────────────┐
│ m9p_build_tattach │─── TX ──────────► PA3 (USART2 RX)     │
│ m9p_build_twalk   │                  │ uart_transport.c   │
│ m9p_build_tread   │◄── RX ────────── PA2 (USART2 TX)     │
│ m9p_parse_rattach │                  │ mini9p_server.c    │
│ ...               │                  │ -> local backend   │
└───────────────────┘                  └────────────────────┘
```

硬件连接：

```text
USB-TTL TX -> STM32 PA3 / USART2_RX
USB-TTL RX -> STM32 PA2 / USART2_TX
USB-TTL GND -> STM32 GND
```

注意 USB-TTL 建议使用 3.3V TTL 电平。USART2 当前计划使用 1Mbaud；如果首测不稳定，可以先降到 115200 或 921600 排查。

PC 端实现建议：

```text
第一版已用 C 写 master emulator
目录为 tools/pc_master_emulator/
复用 pwos-slave/User/mini9p/mini9p_protocol.c/.h 构造 T*、解析 R*
串口读帧时按 mini9p 长度字段读取完整 frame
```

构建方式：

```bash
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
```

运行方式：

```bash
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000
```

PC 收帧流程必须和协议一致：

```text
先读 4 字节：'9' 'P' frame_len_le16
计算 total_len = frame_len + 6
继续读满剩余字节
调用 m9p_decode_frame()
再调用 m9p_parse_r*
```

最小联调命令序列：

```text
Tattach -> Rattach
Twalk /sys/health -> Rwalk
Topen OREAD -> Ropen
Tread -> Rread("ok\n")
Tclunk -> Rclunk
Twalk /missing -> Rerror ENOENT
Topen bad fid -> Rerror EFID
```

这个阶段的价值：

```text
绕开 ESP32 app_main / cluster_vfs / shell 复杂度
优先确认 STM32 UART 收发和 mini9p_server 状态机
PC 端更容易打印十六进制帧、重发请求和定位 CRC/长度错误
```

## 7. Master 侧联调前置

状态：待完成。

主机侧目前有：

```text
cluster_vfs
cluster_config
mini9p_client
uart_transport
```

但 `cluster_vfs_init()` / `cluster_init_static_nodes()` 还没有接进 `app_main()`。联调前需要在 ESP32 启动链路中初始化：

```c
cluster_vfs_init();
cluster_init_static_nodes();
```

这样 `/mcu1/...` 才能映射到 `g_mcu1_client`，再通过默认 UART transport 发送到 STM32。

## 8. 验证标准

PC 端最小标准：

```text
testbench 可稳定通过
错误路径返回 Rerror，而不是 handler 直接失败
一次 request 只产生一次 response
```

从机端最小标准：

```text
UART 能收到完整 Tattach 并返回 Rattach
UART 能收到 Twalk/Topen/Tread/Tclunk 并返回对应 R*
非法路径返回 Rerror ENOENT
非法 fid 返回 Rerror EFID
```

主从闭环标准：

```text
master 可以 attach 到 STM32
cluster_vfs_read_path("/mcu1/sys/health", ...) 能读到 "ok\n"
多次 open/read/clunk 后 fid 表不泄漏
断线重连后 attach 能重建 session
```

## 9. 后续增强

后续可以逐步补：

```text
目录读取 dirent 编码
Twrite 写控制节点
littlefs 文件节点
设备节点 /dev/*
计算任务节点 /compute/*
更完整的 testbench 错误路径
从机本地 VFS 与未来分布式 route 层
```
