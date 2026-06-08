# PWOS STM32F407 从机

`pwos-slave` 是 STM32F407 从机固件模板。它把 CubeMX 生成的板级支持层和代码库其他部分共用的 Mini9P、mesh、存储、VFS 模块拼装在一起,只留板级 glue 放在本目录。

## 目录结构

```
pwos-slave/
├── Core/                   # STM32CubeMX 生成的 HAL 启动代码、外设初始化、main.c
├── User/
│   ├── app/                # 板级 bring-up 胶水
│   │   ├── mesh_node_mini9p_init.c/.h
│   │   ├── fs_selftest.c/.h
│   │   ├── mesh_diag.c/.h
│   │   ├── vofa_firewater.c/.h
│   │   └── pwos_log.c/.h
│   ├── mesh/               # 从机 mesh service / runtime 集成代码
│   ├── backend/            # 通过 Mini9P 暴露的本地 VFS 后端
│   ├── fs/                 # littlefs 内核和工具
│   ├── drivers/            # 板级驱动
│   └── os/                 # OS 适配
├── Drivers/                # STM32 HAL
├── cmake/                  # 工具链文件
├── CMakeLists.txt
├── CMakePresets.json
├── STM32F407XX_FLASH.ld
├── startup_stm32f407xx.s
├── pwos-slave.ioc          # STM32CubeMX 项目文件
└── build.sh                # 辅助脚本(build/flash/flash-test)
```

复用代码路径:

- Mini9P 协议、client、server 来自 `../pwos-shared/mini9p/`
- Mesh envelope / processer / cluster 来自 `../pwos-shared/mesh/`
- 从机 mesh service / runtime 在 `User/mesh/`
- 后端在 `User/backend/`,被 Mini9P server 加载

## 构建预设

```sh
cmake --preset Debug
cmake --build --preset Debug
```

`Debug` 是正常本地固件配置,默认不启动 Mini9P mesh 串口服务。

F407 Mini9P/mesh bring-up:

```sh
cmake --preset F407Debug
cmake --build --preset F407Debug
```

`F407Debug` 启用 `PWOS_ENABLE_MINI9P_SERIAL=ON`。Mini9P mesh 模式会正常初始化 SDIO 并尝试挂载 SD-backed littlefs 到 `/fs`。

辅助脚本默认使用 `F407Debug`:

```sh
pwos-slave/build.sh build
pwos-slave/build.sh flash
pwos-slave/build.sh flash-test
```

`flash-test` 烧录固件后启动 `tools/pc_master_emulator`,等 emulator 打印等待信息后需要手动复位开发板。

## Mesh 串口模式

开启 `PWOS_ENABLE_MINI9P_SERIAL` 时,`Core/Src/main.c` 切换为 Mini9P 串口联调模式:

```c
MX_GPIO_Init();
MX_USART1_UART_Init();
MX_USART2_UART_Init();
MX_SDIO_SD_Init();
MX_USART3_UART_Init();
MX_UART4_UART_Init();
MX_USART6_UART_Init();
mesh_node_mini9p_init();
while (1) {
    mesh_node_service_poll_once();
}
```

正常 VOFA/firewater 和 fs 自检循环在该模式下被禁用。所有启用的 mesh UART 只承载 Mini9P/mesh 二进制帧,不输出 VOFA 文本或 fs report,避免污染协议流。

当前 F407 mesh UART 配置:

- `USART1`: `PA9=TX`, `PA10=RX`
- `USART2`: `PA2=TX`, `PA3=RX`
- `USART3`: `PB10=TX`, `PB11=RX`
- `UART4`: `PA0=TX`, `PA1=RX`
- `USART6`: `PC6=TX`, `PC7=RX`

这些端口均运行在 `1000000` 波特率。`mesh_node_mini9p_init()` 会根据 HAL UART 初始化状态自动把已启用端口加入 mesh service。master 或另一块 slave 可以接到上面任意一个端口,bootstrap REGISTER 会广播到全部已启用端口,收到 ASSIGN 的端口会成为当前上游 control-plane 端口。

`UART5` 的固定引脚是 `PC12=TX`、`PD2=RX`,与 SDIO 的 `SDIO_CK/CMD` 冲突。默认为了挂载 SD-backed `/fs` 不启用 UART5；如果确实要测试六串口 mesh,可以用 `PWOS_ENABLE_UART5_MESH=ON`,这会跳过 SDIO 初始化并使 `/fs` 不可用。

## PC 主控模拟器

```sh
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000 1
```

第三个参数是等待的节点数量,测试双从机链路时使用 `2`。

## 注意事项

- 不要为从机添加静态邻居地址配置。从机 `addr -> port` 映射在运行时通过 `NEIGHBOR_PROBE_RESPONSE` 和 pending ASSIGN 回转学习(详见 `User/mesh/README.md`)。
- 共用的 mesh 或 Mini9P 协议变更必须在 `pwos-master-esp32p4/vfs_bridge/test/` 和 `pwos-shared/mesh/module test(on PC)/` 下的 PC 测试中验证。
- 单板 smoke test 推荐命令见 `AGENTS.md`。
