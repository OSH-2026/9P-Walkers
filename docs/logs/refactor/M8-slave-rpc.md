# M8 从机 RPC

日期：2026-06-30  
基线：`f8c368f` 之后的工作树  
状态：unary 阶段已上板通过；streaming 代码与构建完成，等待上板验收

## 范围

- `DATA_RPC` 使用 16 字节固定头，包含 version、kind、flags、call_id、status、
  deadline、service/method/payload 长度；完整性继续由外层 link CRC 保证。
- 完成 unary、streaming、fire-and-forget、cancel、deadline。
- 流式响应由 `STREAM_CHUNK` 和 `STREAM_END` 组成。chunk 固定上限 32 字节、间隔
  10 ms；`STREAM_CHUNK.status` 是从 0 开始的序号，主机检测到丢失、重复或乱序会
  终止 pending，不返回残缺结果。
- P4 采用 512 字节固定缓冲做有界聚合，只有 `STREAM_END` 才唤醒调用者；不在 UART
  RX task 中执行用户回调，也不使用动态分配。
- 主机 RPC 与 mini9P 共用 `session_manager`，pending 按
  `(data_type, src_addr, wire_tag)` 匹配，不会把两类响应串线。
- 主机本地超时后发送同 call_id 的 CANCEL；节点 boot_id 改变仍取消旧 pending。
- 从机 service registry 使用静态内存；延期方法存入 4 项 pending 表，由唯一
  `service_task` 以 10 ms 粒度推进，不 sleep、不阻塞 mesh 控制面。
- 内置方法：`system.ping`、`system.stream`、`system.info`、`system.notify`、
  `system.delay`、`system.fail`。

`docs/refactor_plan.md` 5.4 描述的是 ESP 主机间 TCP/CBOR `host_rpc`，STM32 不参与。
本阶段因此没有把该 TCP framing 搬入 mesh，而是定义 MCU 侧固定头 `DATA_RPC`；
payload 保持不透明，后续可以在方法级承载 CBOR。lttit `ccrpc` 依赖动态分配和阻塞
transport，不适合当前 STM32 静态 service task，因此未直接接入默认固件。

## 已执行验证

- RPC codec PC 测试：request/response/cancel/stream、retag、畸形帧。
- RPC service PC 测试：ping、not-found、单向通知、延期完成、无 deadline、远端
  deadline、cancel、三 chunk + END、流式 deadline。
- host session/client PC 测试：typed pending、unary、notify、本地 deadline 后自动
  CANCEL、流式聚合、流式超时 CANCEL、chunk 乱序拒绝。
- command service PC 测试：`rpc`、`stream`、`notify`、deadline 参数和远端错误。
- 全部现有 PC 测试通过；host coordinator 和 mesh2 可执行测试手动运行通过。
- F407 Debug：RAM 69544 B / 128 KiB（53.06%），FLASH 92744 B / 512 KiB
  （17.69%）。
- ESP32-P4：固件 `0x89c80`，app 分区剩余 `0x76380`（46%）。
- `git diff --check` 通过。

## Unary 上板结果

2026-06-30，用户在两节点拓扑确认：

- `rpc mcu1 system.ping hello-mcu1` 返回 `hello-mcu1`。
- `rpc mcu2 system.ping hello-mcu2` 返回 `hello-mcu2`。
- `notify mcu1 system.notify event` 返回 `queued`。
- `rpc mcu2 system.delay 500 --deadline=50` 返回 `remote deadline (3)`。
- 上述操作后 mini9P health 和 RPC ping 均保持正常。

## Streaming 上板验收

拓扑保持：`P4 -> MCU1 USART2 -> MCU1 USART1 -> MCU2 USART1`。两个节点重新上线且
周期 health 稳定后，在 WebShell 依次执行：

```text
stream mcu1 system.stream 0123456789012345678901234567890123456789012345678901234567890123456789
stream mcu2 system.stream abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789
stream mcu2 system.delay 100 --deadline=500
stream mcu2 system.delay 500 --deadline=50
cat /host/sys/sessions
cat /mcu1/sys/sessions
cat /mcu2/sys/sessions
```

期望：

1. 两条 stream 原样回显，分别报告 `stream chunks=3 bytes=70` 和
   `stream chunks=2 bytes=62`。
2. 100 ms delay 返回 `done` 和 `stream chunks=1 bytes=4`；500 ms delay 返回
   `remote deadline (3)`，不输出残缺 payload。
3. P4 `/host/sys/sessions` 的 stream、chunk、stream_done 计数增长，pending 无持续
   占用；两块 MCU 的 `/sys/sessions` stream request/chunk/end 计数对应增长。
4. stream 等待期间控制面 hello/lease/route 持续，P4 周期 mini9P health 不超时。
5. 断开 mcu2 后调用得到本地 deadline/no-route；恢复后 mcu1、mcu2 的 RPC 和
   mini9P 都能再次成功，不需要重启 P4。

以上通过后，M8 完成并进入 M9 Job System。
