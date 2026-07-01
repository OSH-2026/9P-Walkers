# PWOS ESP32-S3 主机

ESP32-S3 固件同时承担 STM32 coordinator、WiFi host RPC peer 和 LLM 推理节点。

## 运行时

- UART1 默认 TX=17、RX=18、1 Mbaud，连接一个 STM32 子树。
- WiFi STA 默认连接 `pwos-network`，密码 `pwos-network`。
- mDNS 发布 `_pwos._tcp`，TCP/9909 运行 host RPC。
- 复用 P4 的 coordinator、session、cluster VFS、slave RPC、Job 和 host RPC 模块。
- 本地运行 `llm_engine`、`inference_runtime` 和 `dist_inference_service`。
- 默认 host priority 200；P4 默认 300，但更高 epoch 仍优先。

## 目录

```text
host_net/      WiFi STA、mDNS
inference/     S3 本地 LLM 引擎和分布式推理入口
main/          app_main、Kconfig、组件构建
model/         SPIFFS 内置模型和 tokenizer
```

共享的主机模块目前直接从 `../pwos-master-esp32p4` 编译。修改公共主机行为时必须同时
构建 P4 和 S3，避免平台条件分支只在一侧通过。

## 配置

```text
idf.py menuconfig
  -> 9P-Walkers ESP32-S3
     -> WiFi SSID/password
     -> STM32 UART port/TX/RX/baud
```

## 构建和烧录

```bash
source /home/hb/.espressif/v6.0/esp-idf/export.sh
cd pwos-master-esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

启动日志应依次出现 coordinator、WiFi IP、host RPC 和 inference 状态。网络服务失败时
不会停止 UART coordinator。
