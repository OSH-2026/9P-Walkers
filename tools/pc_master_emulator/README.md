# PC Master Emulator

PC 端 Mini9P 主控模拟器，用于通过 USB-TTL 串口联调 STM32 从机。

该工具复用 `pwos-slave/User/mini9p/mini9p_protocol.c` 构造和解析 Mini9P 帧。当前只覆盖一条固定的 smoke test 流程，用来验证从机侧 `uart_transport + mini9p_server + local_vfs` 是否连通。

## 构建

```bash
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
```

## 运行

```bash
tools/pc_master_emulator/build/pc_master_emulator <serial-dev> [baud]
```

示例：

```bash
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 115200
```

如果省略波特率，工具默认使用 `1000000`。当前 `pwos-slave-stm32f411`
工程的 USART1 配置为 `115200`，运行时需要显式传入 `115200`，或者同步修改
固件里的 `huart1.Init.BaudRate`。

## 测试流程

工具启动后会依次执行：

1. `Tattach`
2. `Twalk /sys/health`
3. `Topen OREAD`
4. `Tread /sys/health`
5. `Tclunk`
6. `Twalk /missing`，期望返回 `ENOENT`
7. `Topen bad fid`，期望返回 `EFID`

成功标准是 `/sys/health` 读到 `ok\n`，并打印：

```text
pc_master_emulator: ok
```

## 连接约定

- USB-TTL TX -> STM32 USART1 RX，通常为 `PA10`
- USB-TTL RX -> STM32 USART1 TX，通常为 `PA9`
- GND -> GND
- 使用 3.3V TTL 电平

从机固件需要开启 Mini9P 串口联调模式，例如使用 `PWOS_ENABLE_MINI9P_SERIAL` 构建。该模式下对应 USART 应由 Mini9P 二进制帧独占，不应混入 VOFA 或其他文本日志。

## 当前边界

- 仅支持 Linux/POSIX 串口，使用 `termios`。
- 当前是硬件联调工具，不替代 backend/server 单元测试。
- 当前流程固定，不提供交互式 Mini9P shell。
- 如果出现 `serial read timeout`，优先检查从机主循环是否阻塞、USART2 是否被日志污染、线序/电平/供电是否稳定。
