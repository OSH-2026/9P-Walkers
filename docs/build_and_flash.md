# 构建、烧录与测试

## 1. 依赖

- CMake 3.22+、Ninja。
- GNU Arm Embedded Toolchain 和 OpenOCD。
- ESP-IDF 6.0 环境。
- STM32CubeMX 生成文件已提交到各 STM32 工程。

## 2. STM32F407

```bash
cd pwos-slave
cmake --preset F407Debug
cmake --build --preset F407Debug
```

`F407Debug` 启用当前 mesh 串口配置。烧录：

```bash
PRESET=F407Debug pwos-slave/build.sh flash
```

也可直接使用：

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program pwos-slave/build/F407Debug/pwos-slave.elf verify reset exit"
```

## 3. STM32F429

```bash
cd pwos-slave-stm32f429
cmake --preset Debug
cmake --build --preset Debug

openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program build/Debug/pwos-slave-stm32f429.elf verify reset exit"
```

F429 固件包含与 F407 相同的通信/服务/计算栈，以及 SDRAM、LTDC、ILI9341 和
`/display/tile`。

## 4. ESP32-P4

```bash
source /home/hb/.espressif/v6.0/esp-idf/export.sh
idf.py -C pwos-master-esp32p4 build
idf.py -C pwos-master-esp32p4 -p <PORT> flash monitor
```

默认硬件是 ESP32-P4 Function EV Board：IP101 PHY 地址 1，reset GPIO 51；
coordinator UART1 使用 TX=37、RX=38、1 Mbaud。

## 5. ESP32-S3

```bash
source /home/hb/.espressif/v6.0/esp-idf/export.sh
idf.py -C pwos-master-esp32s3 build
idf.py -C pwos-master-esp32s3 -p <PORT> flash monitor
```

默认配置：

```text
WiFi SSID/password: pwos-network / pwos-network
STM32 UART: UART1, TX=17, RX=18, 1 Mbaud
```

可用 `idf.py -C pwos-master-esp32s3 menuconfig` 修改 `9P-Walkers ESP32-S3` 菜单。

## 6. PC 单元测试

每个测试工程独立构建到 `/tmp`，避免把生成物写入源码树。

```bash
cmake -S pwos-shared/link/tests -B /tmp/pwos-link-tests
cmake --build /tmp/pwos-link-tests
ctest --test-dir /tmp/pwos-link-tests --output-on-failure

cmake -S pwos-shared/mesh2/tests -B /tmp/pwos-mesh2-tests
cmake --build /tmp/pwos-mesh2-tests
ctest --test-dir /tmp/pwos-mesh2-tests --output-on-failure

cmake -S pwos-shared/rpc/tests -B /tmp/pwos-rpc-tests
cmake --build /tmp/pwos-rpc-tests
ctest --test-dir /tmp/pwos-rpc-tests --output-on-failure

cmake -S pwos-shared/job/tests -B /tmp/pwos-job-tests
cmake --build /tmp/pwos-job-tests
ctest --test-dir /tmp/pwos-job-tests --output-on-failure

cmake -S pwos-shared/host_rpc/tests -B /tmp/pwos-host-rpc-protocol-tests
cmake --build /tmp/pwos-host-rpc-protocol-tests
ctest --test-dir /tmp/pwos-host-rpc-protocol-tests --output-on-failure

cmake -S pwos-master-esp32p4/host_coordinator/tests -B /tmp/pwos-host-coordinator-tests
cmake --build /tmp/pwos-host-coordinator-tests
ctest --test-dir /tmp/pwos-host-coordinator-tests --output-on-failure

cmake -S pwos-master-esp32p4/host_sessions/tests -B /tmp/pwos-host-session-tests
cmake --build /tmp/pwos-host-session-tests
ctest --test-dir /tmp/pwos-host-session-tests --output-on-failure

cmake -S pwos-master-esp32p4/host_api/tests -B /tmp/pwos-host-api-tests
cmake --build /tmp/pwos-host-api-tests
ctest --test-dir /tmp/pwos-host-api-tests --output-on-failure

cmake -S pwos-master-esp32p4/host_shell/tests -B /tmp/pwos-host-shell-tests
cmake --build /tmp/pwos-host-shell-tests
ctest --test-dir /tmp/pwos-host-shell-tests --output-on-failure

cmake -S pwos-master-esp32p4/host_rpc/tests -B /tmp/pwos-host-rpc-endpoint-tests
cmake --build /tmp/pwos-host-rpc-endpoint-tests
ctest --test-dir /tmp/pwos-host-rpc-endpoint-tests --output-on-failure

cmake -S pwos-slave/User/backend/test -B /tmp/pwos-backend-tests
cmake --build /tmp/pwos-backend-tests
ctest --test-dir /tmp/pwos-backend-tests --output-on-failure

cmake -S pwos-slave/User/service/tests -B /tmp/pwos-service-tests
cmake --build /tmp/pwos-service-tests
ctest --test-dir /tmp/pwos-service-tests --output-on-failure

cmake -S pwos-slave/User/compute/tests -B /tmp/pwos-compute-tests
cmake --build /tmp/pwos-compute-tests
ctest --test-dir /tmp/pwos-compute-tests --output-on-failure
```

mini9P client 的轻量 host test：

```bash
cc -std=c11 -Wall -Wextra -Werror \
  -I pwos-shared/mini9p \
  pwos-shared/mini9p/test_mini9p_client_host.c \
  pwos-shared/mini9p/mini9p_client.c \
  pwos-shared/mini9p/mini9p_protocol.c \
  -o /tmp/pwos-mini9p-client-test
/tmp/pwos-mini9p-client-test
```

## 7. 上板验收

单主机链式拓扑：

```text
ESP32 -> MCU1 -> MCU2 -> MCU3
```

至少检查：

```text
ls /
cat /mcu1/sys/health
cat /mcu2/sys/routes
rpc mcu2 system.ping hello
job submit mcu3 matmul
job result <id>
```

多主机时，P4 和 S3 接入同一可信 LAN，分别连接自己的 STM32 子树。两边 `hosts`
应看到相同 leader 和全局节点表，跨 owner 的 `cat /mcuN/sys/health` 应成功。

## 8. 常见问题

- UART 无响应：检查 TX/RX 交叉、共地、3.3 V TTL 和 1 Mbaud。
- STM32 DMA 不收包：确认 RX DMA、UART global IRQ 和 ReceiveToIdle 回调已启用。
- S3 不联网：检查 SSID/密码和 WPA2 配置。
- P4 无 WebShell：先看 Ethernet DHCP、IP101 reset GPIO 和 mDNS 日志。
- ESP-IDF 构建找不到环境：重新执行 IDF 6.0 `export.sh`。
