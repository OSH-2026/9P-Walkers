# 9P-Walkers

9P-Walkers 是一个面向异构 MCU 集群的实验系统。当前重构已经完成，主线由
ESP32-P4/ESP32-S3 主机、STM32F407/STM32F429 节点和共享协议栈组成。

## 当前架构

```text
                         主机间 IP 平面
              mDNS + TCP/9909 + bounded CBOR RPC

    ESP32-P4 (Ethernet) <-----------------> ESP32-S3 (WiFi)
           |                                      |
           | UART 1 Mbaud                         | UART 1 Mbaud
           v                                      v
      STM32 子树                              STM32 子树
           \________ link v2 + mesh2 + data plane ________/

数据面:
  DATA_MINI9P  远端文件与诊断命名空间
  DATA_RPC     MCU unary/stream/oneway/cancel RPC
  DATA_JOB     可查询、取消、重试的计算任务
```

STM32 节点可以通过多个 UART 组成多跳拓扑。主机负责地址租约、拓扑和路由下发；
节点只维护端口状态、本地邻居和简化转发表。

## 已实现能力

- STM32 UART ReceiveToIdle DMA、固定块池、FreeRTOS 队列和独立链路/控制/服务任务。
- `LINK_HELLO/ACK` 端口发现，节点注册、地址租约、链路上报和多跳路由。
- `/mcuN/...` 统一命名空间和 `/host/sys/...` 主机可观测性。
- 并发 mini9P、MCU RPC、Job System，以及按数据类型隔离的 pending 表。
- P4 有线 WebShell；S3 WiFi STA；主机通过 mDNS 自动发现并选举 leader。
- 跨主机节点 read/write、全局 `mcuN` 命名和拓扑同步。
- STM32 上的 hash、vector add、matmul、Mandelbrot、raytrace tile kernel。
- P4 Lua 调度器把 240x320 smallpt 场景分片到多个 MCU，F429 LCD 接收 tile 并显示。
- P4/S3 上的本地 LLM 推理运行时和分布式推理服务骨架。

## 仓库目录

```text
pwos-shared/                 共享 link/mesh2/mini9P/RPC/Job/host RPC/render 协议
pwos-master-esp32p4/         P4 coordinator、WebShell、host RPC、Lua、推理
pwos-master-esp32s3/         S3 coordinator、WiFi、host RPC、推理
pwos-slave/                  STM32F407 FreeRTOS 节点
pwos-slave-stm32f429/        STM32F429 节点与 240x320 LCD
docs/                        当前架构、协议、构建和历史记录
Lab4/                        独立的 Linux 分布式推理实验
```

## 快速构建

```bash
# STM32F407
cd pwos-slave
cmake --preset F407Debug
cmake --build --preset F407Debug
cd ..

# STM32F429
cd pwos-slave-stm32f429
cmake --preset Debug
cmake --build --preset Debug
cd ..

# ESP32-P4 / ESP32-S3
source /home/hb/.espressif/v6.0/esp-idf/export.sh
idf.py -C pwos-master-esp32p4 build
idf.py -C pwos-master-esp32s3 build
```

详细命令见 [docs/build_and_flash.md](docs/build_and_flash.md)。

## 文档入口

- [系统架构](docs/architecture.md)
- [协议规范](docs/protocol_spec.md)
- [链路与 UART 运行时](docs/transport_abstraction.md)
- [主机网络与多主机](docs/host_network.md)
- [从机运行时](docs/slave_mesh_runtime.md)
- [重构完成记录](docs/refactor_plan.md)
- [里程碑验收记录](docs/logs/refactor/README.md)

`docs/logs/`、调研报告和 ADR 用于保留历史背景；现行行为以源码头文件和上述当前
文档为准。
