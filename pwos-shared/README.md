# pwos-shared

共享目录只保存与具体 HAL/ESP-IDF 无关的协议和算法。

```text
link/       link frame v2、CRC、增量 parser
mesh2/      注册、lease、链路、路由和 host advertise payload
mini9p/     mini9P protocol/client/server
rpc/        STM32 DATA_RPC 内层协议
job/        STM32 DATA_JOB 内层协议
host_rpc/   ESP32 主机间 CBOR RPC 和 leader election
render/     STM32 smallpt raytrace tile kernel
```

旧 `pwos-shared/mesh` 和未接入固件的 `csc` 实验副本已删除。硬件 UART/DMA 实现在
各固件工程中，不再由共享目录提供跨平台 transport。

## 测试

```bash
for suite in link mesh2 rpc job host_rpc; do
  cmake -S "pwos-shared/$suite/tests" -B "/tmp/pwos-$suite-tests"
  cmake --build "/tmp/pwos-$suite-tests"
  ctest --test-dir "/tmp/pwos-$suite-tests" --output-on-failure
done
```

协议字段和 wire 上限见 `docs/protocol_spec.md`。
