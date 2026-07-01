# PWOS ESP32-P4 主机

ESP32-P4 是默认 coordinator、Ethernet 网关、WebShell、Lua 调度和推理节点。

## 主要职责

- UART1 TX=37、RX=38、1 Mbaud 接入 STM32 子树。
- 处理 HELLO、REGISTER、ASSIGN、lease、link state 和 route update。
- `session_manager` 并发管理 mini9P、MCU RPC 和 Job typed pending。
- `cluster_vfs` 暴露 `/mcuN/...`，`host_observability` 暴露 `/host/sys/...`。
- IP101 Ethernet 获取 DHCP 地址并发布 `pwos-xxxx.local`。
- HTTP/WebSocket 提供 WebShell。
- TCP/9909 + `_pwos._tcp` 提供主机发现、选主、拓扑同步和跨主机 read/write。
- Lua 调度器把 raytrace tile 分派给在线 STM32，并写入 F429 LCD。
- 本地 LLM 推理和主机间分布式推理服务。

## 目录

```text
coordinator_runtime/  UART coordinator 和唯一 RX task
host_coordinator/     节点、lease、拓扑、路由
host_sessions/        mini9P/RPC/Job pending 和 session
host_api/             cluster VFS、主机可观测性
slave_rpc/            STM32 DATA_RPC client
host_jobs/            Job manager 和命令
host_rpc/             TCP/mDNS/CBOR 主机间平面
host_shell/           命令服务
host_net/             IP101 Ethernet
web/                  HTTP/WebSocket
lua/ render/          Lua 5.4 子集和渲染调度器
inference/ model/     LLM 引擎、服务和 SPIFFS 模型
main/                 ESP-IDF component
```

## 构建

```bash
source /home/hb/.espressif/v6.0/esp-idf/export.sh
cd pwos-master-esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

## WebShell

DHCP 成功后访问串口日志中的 IP 或 `http://pwos-xxxx.local/`。

```text
ls /
cat /mcu1/sys/health
cat /mcu2/sys/routes
cat /host/sys/hosts
hosts
net status

rpc mcu1 system.ping hello
stream mcu2 system.stream 0123456789
notify mcu1 system.notify event

job caps mcu1
job submit mcu1 hash hello
job submit mcu2 matmul
job status <id>
job result <id>
job cancel <id>
job retry <lost-id>

fault mcu1 status
fault mcu1 drop port 0 20
fault mcu1 clear
```

## Lua 分片渲染

`render/whitted_scheduler.lua` 当前参数：

```text
image:       240 x 320
tile:        16 x 7
samples:     4
max depth:   4
kernel:      raytrace_tile
display:     first node exposing /display/status
```

Lua runtime 枚举可达 MCU，每个节点同时运行一个 tile job。tile 结果是 RGB565，
通过 mini9P 写入 F429 `/display/tile`。

```text
cat /mcu3/display/status
cat /mcu3/compute/jobs
```

## PC 测试

主机模块测试命令见 `docs/build_and_flash.md`。修改共享主机模块后还必须构建
`pwos-master-esp32s3`，因为 S3 直接复用这些源码。
