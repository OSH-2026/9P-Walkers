# PWOS STM32F407 从机

`pwos-slave` 是 STM32F407 从机固件模板。它将 CubeMX 生成的板级支持层与代码库其他部分共用的 Mini9P、mesh、存储和 VFS 模块结合在一起。

## 目录结构

- `Core/` 包含 STM32CubeMX 生成的 HAL 启动代码、外设初始化和 `main.c`。
- `User/app/` 包含板级bring-up胶水代码，如 `mesh_node_mini9p_init.c`、`fs_selftest.c` 和可选的mesh诊断功能。
- `User/mesh/` 包含从机mesh服务/运行时集成代码。它将共用的mesh代码绑定到 STM32 UART传输层。
- `User/backend/` 包含通过 Mini9P暴露的节点本地 VFS 后端。
- `User/fs/` 和 `User/drivers/storage/` 包含 littlefs 和存储端口。
- 共用的协议和路由代码从 `../pwos-shared/` 引入。

## 构建预设

使用本目录的 CMake 预设：

```sh
cmake --preset Debug
cmake --build --preset Debug
```

`Debug` 是正常的本地固件配置。默认不启动 Mini9P mesh串口服务。

ZGT6 开发板 Mini9P/mesh bring-up：

```sh
cmake --preset ZGT6Debug
cmake --build --preset ZGT6Debug
```

`ZGT6Debug` 启用：

- `PWOS_BOARD_ZGT6=ON`
- `PWOS_ENABLE_MINI9P_SERIAL=ON`
- `PWOS_SKIP_LFS_MOUNT=ON`

辅助脚本默认使用 `ZGT6Debug`：

```sh
pwos-slave/build.sh build
pwos-slave/build.sh flash
pwos-slave/build.sh flash-test
```

`flash-test` 烧录固件后启动 `tools/pc_master_emulator`，等待 emulator 打印等待信息后需要手动复位开发板。

## Mesh串口模式

当设置 `PWOS_ENABLE_MINI9P_SERIAL` 时，`main.c` 启动 `mesh_node_mini9p_init()`，然后在主循环中轮询 `mesh_node_service_poll_once()`。禁用了正常的 VOFA/firewater 和文件系统自检循环。

当前 F407 mesh UART 配置：

- `port0`：`USART2`（`PA2 TX`，`PA3 RX`），用于主机/PC 上行链路。
- `port1`：`USART1`（`PA9 TX`，`PA10 RX`），用于下行从机链路。

两个端口均运行在 `1000000` 波特率。单节点 PC 冒烟测试时，将 PC USB-UART 适配器连接到 `USART2`。

## PC主机模拟器

从代码库根目录构建并运行 PC 主机模拟器：

```sh
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000 1
```

第三个参数是等待的节点数量。测试双从机链路时使用 `2`。

## Mesh 诊断

mesh诊断默认不编译。仅在调试板级 bring-up 时启用：

```sh
cmake --preset ZGT6Debug -DPWOS_ENABLE_MESH_DIAG=ON
cmake --build --preset ZGT6Debug
```

诊断信息通过 `USART1` 阻塞式打印。由于 `USART1` 同时也是 `mesh port1`，这可能干扰时序并在二进制 mesh 帧中混入文本。正常 mesh 测试和链路中继测试时请保持 `PWOS_ENABLE_MESH_DIAG=OFF`。

## 注意事项

- 不要为从机添加静态邻居地址配置。从机 `addr -> port` 映射在运行时学习。
- 共用的 mesh 或 Mini9P 协议变更应在 `pwos-master-esp32p4/vfs_bridge/test/` 和 `pwos-shared/mesh/module test(on PC)/`下的 PC 测试中验证。