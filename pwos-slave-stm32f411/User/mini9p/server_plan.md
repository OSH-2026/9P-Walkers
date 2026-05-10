# Mini9P Server 当前计划

## 目标

先跑通 STM32 从机侧 Mini9P 闭环：

```text
PC/ESP32 master
  -> UART
  -> uart_transport
  -> mini9p_server
  -> backend/local_vfs
  -> mini9p_server
  -> UART response
```

当前阶段以 PC Master Emulator 上板联调为主，先验证从机侧链路稳定，再接 ESP32 master。

## 模块边界

```text
mini9p_protocol  只负责帧编解码、T* 解析、R* 构造
mini9p_server    负责会话状态、fid 生命周期、请求分发、Rerror
backend/local_vfs 负责 stat/open/read/write/clunk 的本地资源实现
uart_transport   负责收发完整 Mini9P frame
mini9p_service   负责上板组装 local_vfs + server + UART
```

`mini9p_server` 不直接绑定 littlefs。后续 littlefs 应作为 backend provider 接入。

## 当前状态

已完成：

- `mini9p_protocol`：server 方向 T* 解析和 R* 构造。
- `mini9p_server`：attach、walk、open、read、write、stat、clunk 状态机。
- `m9p_server_ops`：server 到 backend 的抽象接口。
- `local_vfs v1`：只读虚拟节点 `/`、`/sys`、`/sys/health`。
- `mini9p_service`：组装 `local_vfs + mini9p_server + uart_transport`。
- `tools/pc_master_emulator`：PC 端串口 master 模拟器。
- PC 单元测试和首次 STM32F407ZGT6 上板串口 smoke test。

当前从机可通过 Mini9P 读取：

```text
/sys/health -> "ok\n"
```

## 从机串口模式

开启 `PWOS_ENABLE_MINI9P_SERIAL` 后，`Core/Src/main.c` 切换为 Mini9P 串口联调模式：

```text
MX_GPIO_Init
MX_USART2_UART_Init
mini9p_service_init
while (1) mini9p_service_poll_once
```

该模式下 USART2 只承载 Mini9P 二进制帧，不输出 VOFA 文本或 fs report，避免污染协议流。

## PC Master Emulator

构建：

```bash
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
```

运行：

```bash
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000
```

最小测试序列：

```text
Tattach
Twalk /sys/health
Topen OREAD
Tread -> "ok\n"
Tclunk
Twalk /missing -> Rerror ENOENT
Topen bad fid -> Rerror EFID
```

## PC 单元测试

server testbench：

```bash
cmake -S pwos-slave/User/mini9p/test -B pwos-slave/User/mini9p/test/build
cmake --build pwos-slave/User/mini9p/test/build
pwos-slave/User/mini9p/test/build/mini9p_server_test
```

backend testbench：

```bash
cmake -S pwos-slave/User/backend/test -B pwos-slave/User/backend/test/build
cmake --build pwos-slave/User/backend/test/build
pwos-slave/User/backend/test/build/local_vfs_test
```

## 下一步

- 继续观察上板串口稳定性，重点排查偶发 `serial read timeout`。
- 将 ESP32 master 侧 `cluster_vfs/mini9p_client` 接入启动链路。
- 在从机侧保持 `local_vfs v1` 稳定后，再规划 littlefs provider 接入。
