# PC Master Emulator

PC 端 Mini9P 主控模拟器，用于通过 USB-TTL 串口联调 STM32 从机。

该工具复用共享 Mini9P、mesh runtime 和 cluster_vfs 模块，通过串口完成节点注册、命名、挂载和健康检查。默认等待两个节点，用来验证从机侧 `mesh_node_runtime + mini9p_server + local_vfs` 是否连通。

当前定位：

- 这是 PC 端 host 模拟器，可用于 `PC host <-> slave A <-> slave B` 这类串联硬件联调。
- 它覆盖第一次 `REGISTER(src=0xff)`、主机回 `ASSIGN`、节点第二次确认 `REGISTER(src=正式地址)`、再通过 `cluster_vfs` 做 Mini9P 访问。
- 它不模拟从机侧 `NEIGHBOR_PROBE_*`、pending REGISTER、ASSIGN 回转或 `LINK_STATE` 上报；这些必须由真实从机固件完成。
- 它不会在 confirmed REGISTER 时假设节点与 host 直连；主机 topology 只接受从机实际上报的 `LINK_STATE`。
- 它会打印 `LINK_STATE` 和 `ROUTE_UPDATE`，用于观察 host runtime 是否按有向 topology 下发路由。

## 构建

```bash
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
```

## 运行

```bash
tools/pc_master_emulator/build/pc_master_emulator <serial-dev> [baud] [node-count]
```

单板 smoke test：

```bash
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000 1
```

两从机串联联调：

```bash
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000 2
```

如果省略波特率，工具默认使用 `1000000`。当前 `pwos-slave` 与
`pwos-slave-stm32f411` 的 UART 初始化均配置为 `1000000`。

如果省略 `node-count`，工具默认等待 2 个节点。节点按本次运行内的注册顺序分配：

- 第 1 个新 UID：`mcu1`，地址 `0x11`
- 第 2 个新 UID：`mcu2`，地址 `0x22`
- 后续依次为 `mcu3/0x33` 等，最大 14 个节点

## 测试流程

工具启动后会依次执行：

1. 接收每个节点的 bootstrap `REGISTER`
2. 按注册顺序发送 `ASSIGN mcuN addr=0xNN`
3. 等待每个节点用正式地址再次 `REGISTER`
4. 接收从机上报的 `LINK_STATE`，由 host runtime 计算并下发 `ROUTE_UPDATE`
5. 对每个目标节点执行 `cluster_vfs_attach("mcuN")`
6. 读取 `/mcuN/sys/health`

成功标准是每个目标节点的 `/sys/health` 都读到 `ok\n`，并打印：

```text
pc_master_emulator: ok
```

## 连接约定

当前默认从机集成：

- `pwos-slave/User/app/mesh_node_mini9p_init.c` 默认只把 `huart2` 加入 `mesh_node_service`。
- 当前 F407/F411 的 `USART2` 均为 `1000000 8N1`。
- `USART2 TX = PA2`，`USART2 RX = PA3`。

因此单板直连可以直接使用默认固件；两从机串联时，slave A 必须先改成多端口 mesh 配置，把第二个 UART 也加入 `mesh_config.ports[]`，否则 A 没有下游端口可以接 slave B。

直连 smoke test：

- USB-TTL TX -> STM32 Mini9P UART RX；当前 F407/F411 联调口为 `USART2 RX = PA3`
- USB-TTL RX -> STM32 Mini9P UART TX；当前 F407/F411 联调口为 `USART2 TX = PA2`
- GND -> GND
- 使用 3.3V TTL 电平

两从机串联联调：

- PC USB-TTL 接 slave A 的上游 mesh UART，默认可用 `USART2 PA2/PA3`。
- slave A 的另一个已启用 mesh UART 接 slave B 的上游 mesh UART。
- slave A/B 固件需要启用多 UART mesh node service。
- slave A/B 负责完成邻居 probe、动态 `addr -> port` 学习和 `LINK_STATE` 上报；PC emulator 只作为 host/controller。

从机固件需要开启 Mini9P 串口联调模式，例如使用 `PWOS_ENABLE_MINI9P_SERIAL` 构建。该模式下对应 USART 应由 Mini9P 二进制帧独占，不应混入 VOFA 或其他文本日志。

## 当前边界

- 仅支持 Linux/POSIX 串口，使用 `termios`。
- 当前是硬件联调工具，不替代 backend/server 单元测试。
- 当前流程固定为注册、挂载和健康检查，不提供交互式 Mini9P shell。
- PC 端只有一个串口 fd，`send_frame(next_hop)` 会把帧写到该串口；多跳转发是否正确由 slave A 的 mesh 路由逻辑决定。
- 如果 `mcu2` 通过 slave A 串联注册但一直不可达，优先检查 slave A/B 是否都上报了真实方向的 `LINK_STATE`，以及日志里 host 是否向对应节点下发了 `ROUTE_UPDATE`。
- 如果出现 `serial read timeout`，优先检查从机主循环是否阻塞、USART2 是否被日志污染、线序/电平/供电是否稳定。
