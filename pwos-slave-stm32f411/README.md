# PWOS STM32F411 从机

`pwos-slave-stm32f411` 是 STM32F411 专用从机固件目录，基于 STM32CubeMX 生成代码，配合代码库其他部分共用的 Mini9P、mesh、存储和 VFS 模块构成完整节点固件。

它与 `pwos-slave`（STM32F407 模板）共享同一套 `User/` 业务代码，仅在板级初始化和 HAL 层面做适配。

## 复用文件与独立文件

### 复用文件（来自 `../pwos-slave/User/`）

```
pwos-slave/User/
├── app/
│   ├── fs_selftest.c/h          # littlefs 自检入口
│   ├── mesh_diag.c/h           # USART1 阻塞式调试输出
│   ├── mesh_node_mini9p_init.c/h  # 板级 mesh/Mini9P/VFS 初始化胶水
│   └── vofa_firewater.c/h      # VOFA 串口日志
├── backend/
│   ├── dev_vfs.c/h             # /dev 虚拟设备节点
│   ├── lfs_vfs.c/h             # littlefs VFS 包装
│   ├── local_vfs.c/h           # Mini9P server 本地 VFS 后端
│   ├── node_vfs.c/h            # 节点级 VFS 入口（组合 sys/dev/lfs）
│   └── sys_vfs.c/h             # /sys 虚拟节点（health/info/routes/log）
├── fs/
│   ├── lfs.c/h                 # littlefs 核心
│   └── lfs_util.c/h            # littlefs 工具函数
└── mesh/
    ├── mesh_node_service.c/h   # 多 UART mesh service（运行时学习 addr→port）
    └── mesh_node_runtime.c/h   # 节点运行时（REGISTER/ASSIGN/neighbor probe）
```

这些文件 **不加修改** 地被 CMakeLists.txt 通过相对路径引用：

```cmake
set(PWOS_SLAVE_USER_DIR ${CMAKE_SOURCE_DIR}/../pwos-slave/User)
set(PWOS_SLAVE_USER_APP_DIR ${PWOS_SLAVE_USER_DIR}/app)
set(PWOS_SLAVE_USER_BACKEND_DIR ${PWOS_SLAVE_USER_DIR}/backend)
set(PWOS_SLAVE_USER_FS_DIR ${PWOS_SLAVE_USER_DIR}/fs)
set(PWOS_SLAVE_USER_MESH_DIR ${PWOS_SLAVE_USER_DIR}/mesh)
```

### 独立文件（STM32F411 专用）

```
pwos-slave-stm32f411/
├── Core/ # STM32CubeMX 生成代码，不应手动编辑
│   ├── Src/main.c              # 调用 mesh_node_mini9p_init()
│   └── Inc/main.h
├── User/
│   ├── app/
│   │   └── mesh_node_mini9p_init.c   # F411 板级初始化（UART handle、端口配置）
│   └── drivers/storage/
│       └── lfs_port.cpp # RAM backed littlefs（F411 无 SD 卡槽）
├── cmake/ # 工具链和 STM32CubeMX CMake 封装
├── User/backend/test/          # 本地 VFS PC 单元测试
├── CMakeLists.txt              # 构建配置（引用 pwos-slave/User 源码）
├── build.sh                    # 辅助脚本：build / flash / test / flash-test
└── pwos-slave-stm32f411.ioc    # STM32CubeMX 项目文件（决定 Core/ 内容）
```

### F411 板级初始化重点

`User/app/mesh_node_mini9p_init.c` 是 F411 独立于 pwos-slave 的核心文件，负责把 STM32CubeMX 生成的 `UART_HandleTypeDef huart1/huart2` 注入到 mesh service：

```c
extern UART_HandleTypeDef huart1;  // 由 CubeMX 生成在 main.c
extern UART_HandleTypeDef huart2;

mesh_config.port_count = 2u;
mesh_config.ports[0].uart_config.uart = &huart2;  // 上游/PC 通信口
mesh_config.ports[1].uart_config.uart = &huart1;  // 下游从机通信口
```

两个文件差异对照：

| 功能 | pwos-slave (F407) | pwos-slave-stm32f411 |
|------|------------------|---------------------|
| mesh init 文件 | `User/app/mesh_node_mini9p_init.c` | 同左（F411 独立版本） |
| mesh_diag | `User/app/mesh_diag.c/h` 引用 | **不引用**，mesh_diag 来自 pwos-slave 共享 |
| node_vfs 回调 | `routes_text_fn` + `log_text_fn` | `routes_text_fn` + `log_text_fn` |
| UART 配置 | 注释掉 USART1 init | 启用 USART1/USART2 双端口 |
| LFS backend | SD 卡 | RAM backed（`PWOS_LFS_PORT_USE_RAM`） |
| 时钟树 | HSI PLL（默认） | **HSE PLL**（SYSCLK 96MHz，APB1 48MHz） |

## 构建预设

F411 只提供 `Debug` 和 `Release` preset,不使用 F407 板级联调 preset。

```sh
# Debug 构建
cmake --preset Debug
cmake --build --preset Debug

# Release 构建
cmake --preset Release
cmake --build --preset Release
```
## 上板方法

### 准备

- 硬件：STM32F411 开发板，USB-UART 适配器（PA2/PA3 即 USART2）
- 工具：`dfu-util`、`openocd` 或 ST-Link、串口终端

### 烧录

```sh
# 方式一：辅助脚本（推荐）
cd pwos-slave-stm32f411
./build.sh flash

# 方式二：手动 dfu-util
dfu-util -a 0 -s 0x08000000:leave -D build/Debug/pwos-slave-stm32f411.bin
```

### 联调

使用 `pwos-slave-stm32f411` 作为**下游从机**（mcu2）时：

```sh
# mcu2 先烧录
pwos-slave-stm32f411/build.sh flash

# mcu1（pwos-slave F407）烧录后启动 emulator等待 2 节点
cd pwos-slave
PC_NODE_COUNT=2 ./build.sh flash-test
```

单板 smoke test：

```sh
pwos-slave-stm32f411/build.sh flash-test # 等待 1 节点
```

默认波特率 **1000000**，设备 `/dev/ttyUSB0`。可通过环境变量覆盖：

```sh
PC_SERIAL_DEV=/dev/ttyUSB1 PC_SERIAL_BAUD=115200 PC_NODE_COUNT=1 ./build.sh flash-test
```

### 上板流程观察

正常上板序列（仅演示流程，非实际调试输出）：

```
[mesh→PC] REGISTER src=0xff dst=0xff seq=1 payload=15
[PC→mesh] ASSIGN mcu2 addr=0x22 uid=11004b0025620107 (34 bytes)
[mesh→PC] REGISTER src=0x22 dst=0xff seq=2 payload=15
[mesh→PC] confirmed mcu2 addr=0x22; waiting for LINK_STATE topology
[mesh→PC] NEIGHBOR_PROBE_REQUEST src=0x22 dst=0xff seq=4 payload=0
[PC→mesh] NEIGHBOR_PROBE_RESPONSE src=0x00 dst=0xff seq=1 payload=0
[mesh→PC] LINK_STATE src=0x22 dst=0x00 seq=5 payload=3
[PC→mesh] ROUTE_UPDATE src=0x00 dst=0x22 seq=12 payload=6
[PC→mesh] MINI9P src=0x00 dst=0x22 seq=14 payload=16
read /mcu2/sys/health: ok
pc_master_emulator: ok
```

## 目录结构概览

```
pwos-slave-stm32f411/
├── Core/ # CubeMX HAL 代码（不手动编辑）
├── Drivers/            # STM32F4xx HAL 驱动
├── User/
│   ├── app/            # F411 板级初始化（mesh_node_mini9p_init.c）
│   ├── backend/         # 复用 pwos-slave/User/backend/
│   ├── drivers/storage/ # F411 RAM backed lfs port
│   └── mesh/            # 复用 pwos-slave/User/mesh/
├── cmake/              # 工具链文件
└── build.sh            # build / flash / test / flash-test
```

## 注意事项

- F411 固件不支持 SD 卡存储，littlefs 使用 RAM backed（`PWOS_LFS_PORT_USE_RAM`）
- F411 不使用 F407 板级联调 preset;构建时只使用本文档列出的 Debug/Release 命令
- USART2（PA2=TX, PA3=RX）是与 PC/host 通信的 mesh 主口，USART1 是下游级联口
- 两个串口均运行在 **1000000 baud**，只传 Mini9P/mesh 二进制帧，不混入 VOFA 文本；时钟树必须配置为 HSE PLL 以保证精度
- 共用代码变更后，F411 和 F407 固件会同时生效；建议先在 F407 验证后再烧录 F411
