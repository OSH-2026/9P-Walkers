# Distributed MCU Cluster OS - 工程架构与开发规划

## 1. 总体架构

```
cluster-os/
│
├── master-esp32p4/
│   ├── main/
│   │   ├── app_main.c
│   │   ├── shell/
│   │   │   ├── shell.c
│   │   │   └── commands.c
│   │   ├── vfs_bridge/
│   │   │   ├── cluster_vfs.c
│   │   │   └── cluster_vfs.h
│   │   ├── rpc_client/
│   │   │   ├── mini9p_client.c
│   │   │   └── mini9p_protocol.h
│   │   ├── lua/
│   │   │   ├── lua_port.c
│   │   │   └── lua_bindings.c
│   │   ├── web/
│   │   │   ├── http_server.c
│   │   │   └── websocket_shell.c
│   │   └── cluster/
│   │       ├── node_manager.c
│   │       └── scheduler.c
│   │
│   ├── components/
│   │   ├── mini9p/
│   │   └── lua/
│   │
│   └── CMakeLists.txt
│
├── slave-stm32f4/
│   ├── Core/
│   │   ├── Inc/
│   │   └── Src/
│   │
│   ├── os/
│   │
│   ├── fs/
│   │   ├── littlefs_port.c
│   │   └── virtual_fs.c
│   │
│   ├── rpc_server/
│   │   ├── mini9p_server.c
│   │   └── file_tree.c
│   │
│   ├── drivers/
│   │   ├── motor.c
│   │   ├── adc_temp.c
│   │   └── gpio_led.c
│   │
│   ├── compute/
│   │   ├── compute_jacobi.c
│   │   ├── compute_matmul.c
│   │   └── compute_nn_stub.c
│   │
│   └── linker/
│       └── stm32f4_flash.ld
│
└── docs/
    ├── architecture.md
    ├── protocol_spec.md
    └── report_outline.md
```

---

## 2. Master (ESP32-P4) 职责

### 2.1 mini9p_client

基于 UART/SPI/WiFi 的极简 RPC 协议客户端。

职责：
- `attach`
- `walk`
- `open`
- `read`
- `write`

不做完整 9P 实现，协议尽量精简。

### 2.2 cluster_vfs

将远程 MCU 文件树映射到统一的命名空间：

```
/mcu1/temperature
/mcu1/motor/speed
/mcu2/compute/jacobi
```

该层负责将 POSIX 风格的 read/write 调用转换为 RPC 请求。

### 2.3 Lua 运行时（仅控制平面）

Lua 仅运行在 Master 上，用途：
- 任务编排
- 并行任务调度
- 自动化脚本

示例用法：
```lua
write("/mcu1/compute/jacobi", params)
result = read("/mcu1/compute/jacobi/result")
```

### 2.4 调度器 (Scheduler)

简单的任务分发器：
- 拆分矩阵块
- 分发到各节点
- 收集结果
- 合并结果

非抢占式，纯协调逻辑。

### 2.5 Web Shell

HTTP 服务器提供：
- 浏览器终端
- 命令转发到本地 Shell
- 通过 WebSocket 实时回显输出

支持的 Shell 命令：
```
ls
cat
echo
upload
run
```

---

## 3. Slave (STM32F4) 职责

### 3.1 RTOS 层

任务调度：
- 通信任务
- 计算任务
- 驱动任务

优先级分离：
1. 通信（最高）
2. 控制 / 电机
3. 计算

### 3.2 虚拟文件树 (Virtual File Tree)

示例文件树：

```
/
├── motor/
│   └── speed
├── temperature
├── compute/
│   ├── jacobi
│   ├── matmul
│   └── result
```

每个文件节点映射到：
- `read` 回调
- `write` 回调

### 3.3 mini9p_server

- 解析收到的数据包
- 路由到对应文件节点
- 执行 read/write 处理函数
- 返回响应

禁止动态内存分配，仅使用静态缓冲区。

### 3.4 计算模块

纯 C 实现，遵循以下规则：
- 不使用 `malloc`
- 固定大小缓冲区
- 运行时间可预测
- 返回状态码 + 结果缓冲区

---

## 4. 通信层

| 阶段 | 介质 | 说明 |
|------|------|------|
| Phase 1 | UART | 最简单、最稳定 |
| Phase 2 | SPI | 更高吞吐 |
| Phase 3 | WiFi Mesh | 可选扩展 |

数据包格式（示例）：

```
| Header | NodeID | Opcode | PathLen | DataLen | Payload |
```

---

## 5. 开发阶段

### Phase 1：单节点验证
- 单个 STM32 节点
- 本地虚拟文件树可正常工作
- 各文件节点的 read/write 回调可独立测试

### Phase 2：主从通信
- 基于 UART 的 RPC 打通 ESP32 与单个 STM32
- 验证 mini9p_client / mini9p_server 协议

### Phase 3：多节点支持
- 支持多个 STM32 Slave
- 命名空间聚合（`/mcu1/...`, `/mcu2/...`）
- 节点发现与管理

### Phase 4：Lua 编排
- 集成 Lua 运行时
- 编写 VFS 绑定
- 实现任务编排示例脚本

### Phase 5：Web Shell
- 搭建 HTTP 服务器
- 实现 WebSocket 终端
- 命令解析与转发

---

## 6. 核心设计原则

1. **一切皆文件**：所有硬件资源和计算任务都抽象为文件节点
2. **控制平面在 Master**：决策、调度、脚本全部放在 ESP32-P4
3. **计算平面在 Slave**：实际运算、驱动控制放在 STM32F4
4. **协议极简**：自定义 mini9p，仅保留必要操作
5. **Slave 行为确定性**：无动态内存、无复杂逻辑、可预测执行时间
6. **计算节点零动态复杂度**：Slave 上禁止使用 malloc、避免递归、避免大块栈分配

---

## 7. 目录与文件说明

| 路径 | 说明 |
|------|------|
| `master-esp32p4/main/shell/` | 本地命令行 Shell |
| `master-esp32p4/main/vfs_bridge/` | 集群虚拟文件系统桥接层 |
| `master-esp32p4/main/rpc_client/` | mini9p RPC 客户端 |
| `master-esp32p4/main/lua/` | Lua 运行时与 VFS 绑定 |
| `master-esp32p4/main/web/` | HTTP + WebSocket 服务器 |
| `master-esp32p4/main/cluster/` | 节点管理与调度器 |
| `master-esp32p4/components/` | 外部组件（mini9p、lua） |
| `slave-stm32f4/Core/` | STM32 HAL / 启动代码 |
| `slave-stm32f4/pwos/` | pwos 组件 |
| `slave-stm32f4/fs/` | LittleFS 移植与虚拟文件树 |
| `slave-stm32f4/rpc_server/` | mini9p RPC 服务端 |
| `slave-stm32f4/drivers/` | 电机、ADC、GPIO 驱动 |
| `slave-stm32f4/compute/` | Jacobi、矩阵乘法、NN Stub |
| `slave-stm32f4/linker/` | 链接器脚本 |
| `docs/` | 架构文档、协议规范、报告大纲 |

---

*本文档为工程初始规划，各阶段细节将在开发过程中随迭代补充到 `protocol_spec.md` 与 `report_outline.md` 中。*
