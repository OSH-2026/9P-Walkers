# PC Master Emulator

PC 端 Mini9P 主控模拟器，用于通过 USB-TTL 串口联调 STM32 从机。

该工具复用共享 Mini9P、mesh runtime 和 cluster_vfs 模块，通过串口完成节点注册、命名、挂载和健康检查。默认等待两个节点，用来验证从机侧 `mesh_node_runtime + mini9p_server + local_vfs` 是否连通。

当前定位：

- 这是单链路 / 直连 smoke test 工具。
- 它覆盖第一次 `REGISTER(src=0xff)`、主机回 `ASSIGN`、节点第二次确认 `REGISTER(src=正式地址)`、再通过 `cluster_vfs` 做 Mini9P 访问。
- 它不模拟从机中继 pending REGISTER、ASSIGN 回转、`NEIGHBOR_PROBE_*`、有向 `LINK_STATE` 双向上报，或主机对全网 all-pairs `ROUTE_UPDATE` 下发。

## 构建

```bash
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
```

## 运行

```bash
tools/pc_master_emulator/build/pc_master_emulator <serial-dev> [baud] [node-count]
```

示例：

```bash
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 115200 2
```

如果省略波特率，工具默认使用 `1000000`。当前 `pwos-slave-stm32f411`
工程的 USART1 配置为 `115200`，运行时需要显式传入 `115200`，或者同步修改
固件里的 `huart1.Init.BaudRate`。

如果省略 `node-count`，工具默认等待 2 个节点。节点按本次运行内的注册顺序分配：

- 第 1 个新 UID：`mcu1`，地址 `0x11`
- 第 2 个新 UID：`mcu2`，地址 `0x22`
- 后续依次为 `mcu3/0x33` 等，最大 14 个节点

## 测试流程

工具启动后会依次执行：

1. 接收每个节点的 bootstrap `REGISTER`
2. 按注册顺序发送 `ASSIGN mcuN addr=0xNN`
3. 等待每个节点用正式地址再次 `REGISTER`
4. 对每个目标节点执行 `cluster_vfs_attach("mcuN")`
5. 读取 `/mcuN/sys/health`

成功标准是每个目标节点的 `/sys/health` 都读到 `ok\n`，并打印：

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
- 当前流程固定为注册、挂载和健康检查，不提供交互式 Mini9P shell。
- 当前控制流假设主机与待测从机直接相连；不验证多跳 bootstrap、邻居发现和 route sync 行为。
- 如果出现 `serial read timeout`，优先检查从机主循环是否阻塞、USART2 是否被日志污染、线序/电平/供电是否稳定。
