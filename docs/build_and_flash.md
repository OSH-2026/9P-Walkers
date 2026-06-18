# 构建与烧录指南

本文档汇总 ESP32-P4 主控、STM32F407 从机以及 PC 模拟器的构建、烧录和监控命令。

## 1. 依赖

### 1.1 通用依赖

- CMake >= 3.22
- Ninja（推荐）或 Make
- Python 3（ESP-IDF pytest 与部分脚本需要）

### 1.2 ESP32-P4 主控

- ESP-IDF（需支持 ESP32-P4），安装后 source 环境使 `IDF_PATH` 可用。
- USB 转串口或 JTAG 调试器，用于 `idf.py flash` / `idf.py monitor`。

### 1.3 STM32F407 从机

- ARM GNU Toolchain：`arm-none-eabi-gcc/g++` 必须在 `PATH` 中。
- OpenOCD。
- ST-Link 调试器。

### 1.4 PC 模拟器

- Linux/POSIX 环境
- `termios` 可用的 USB-TTL 串口
- 普通 GCC（x86_64/arm64）

## 2. ESP32-P4 主控

```bash
cd pwos-master-esp32p4
idf.py build
idf.py flash
idf.py monitor
# 或一条命令
idf.py build flash monitor
```

说明：

- 根 `CMakeLists.txt` 会检查 `IDF_PATH`，未设置则 `FATAL_ERROR`。
- `main/CMakeLists.txt` 把 `pwos-shared/mini9p` 与 `pwos-shared/mesh/{envelope,processer,cluster,transport}` 拉入主组件。
- 同时嵌入 `web/index.html` 作为 WebShell 静态资源。

ESP-IDF pytest：

```bash
cd pwos-master-esp32p4
pytest pytest_hello_world.py
```

## 3. STM32F407 从机（pwos-slave）

### 3.1 构建

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

`F407Debug` preset 会启用 `PWOS_ENABLE_MINI9P_SERIAL`。旧版文档中提到的 `ZGT6Debug` preset 在当前 `CMakePresets.json` 中不存在；需要 ZGT6 专用配置时可手动传入：

```bash
cmake --preset F407Debug -DPWOS_BOARD_ZGT6=ON -DPWOS_SKIP_LFS_MOUNT=ON
```

Mini9P 串口联调口通常固定在 `USART2`：`PA2=TX`、`PA3=RX`、`1000000` baud；该 USART 只传 Mini9P/mesh 二进制帧，不应混入 VOFA 或文本日志。

### 3.2 烧录（OpenOCD）

```bash
cd pwos-slave
./build.sh flash
```

等价命令：

```bash
openocd \
    -f interface/stlink.cfg \
    -f target/stm32f4x.cfg \
    -c "program build/F407Debug/pwos-slave.elf verify reset exit"
```

环境变量可覆盖默认配置：

- `PRESET`：CMake preset，默认 `F407Debug`。
- `OPENOCD_INTERFACE`：默认 `interface/stlink.cfg`。
- `OPENOCD_TARGET`：默认 `target/stm32f4x.cfg`。

## 4. PC 主控模拟器

```bash
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build

# 单板 smoke test
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000 1

# 两从机串联
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000 2
```

省略波特率时默认 `1000000`；省略节点数时默认等待 2 个节点。

## 5. 常用测试构建

### 5.1 Cluster VFS host test

```bash
cmake -S pwos-master-esp32p4/vfs_bridge/test -B pwos-master-esp32p4/vfs_bridge/test/build
cmake --build pwos-master-esp32p4/vfs_bridge/test/build
pwos-master-esp32p4/vfs_bridge/test/build/cluster_vfs_test
```

### 5.2 从机 local VFS backend test

```bash
cmake -S pwos-slave/User/backend/test -B pwos-slave/User/backend/test/build
cmake --build pwos-slave/User/backend/test/build
pwos-slave/User/backend/test/build/local_vfs_test
```

## 6. 常见问题

### 6.1 `IDF_PATH is not set`

ESP-IDF 环境未导出。执行类似：

```bash
. $HOME/esp/esp-idf/export.sh
```

### 6.2 `arm-none-eabi-gcc: command not found`

ARM 工具链未加入 `PATH`。安装后：

```bash
export PATH=$PATH:/path/to/arm-gnu-toolchain/bin
```

### 6.3 STM32 构建失败：找不到 `stm32cubemx`

确保已用 STM32CubeMX 或仓库已生成 `cmake/stm32cubemx/` 目录与 HAL 源文件。

### 6.4 串口通信无响应

- 检查波特率是否为 `1000000`。
- 确认该 USART 未被日志/VOFA 占用。
- 检查线序：TX-RX 交叉、共地、3.3V TTL 电平。
- 使用 `pc_master_emulator` 时，确认从机已上电并发送 `REGISTER`。

### 6.5 OpenOCD 无法连接

- 检查 ST-Link 接线与驱动。
- 确认目标板供电正常。
- 尝试降低接口速度：`-c "adapter speed 1000"`。
