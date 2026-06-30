# M6 主机并发模型验收记录

日期：2026-06-29

## 范围

- `session_manager` 增加固定容量 pending 表，按 `(src_addr, wire_tag)` 匹配响应。
- mini9P 请求使用主机全局 wire tag；返回给协议 client 前恢复本地 tag 并重算 CRC。
- 每节点一个 client mutex：同一节点串行保护 client 缓冲区/fid，不同节点可并发。
- coordinator RX 任务成为唯一 `uart_read_bytes()` 消费者。
- 健康探测移动到独立 worker，只通过 semaphore 等待 pending 完成。
- P4 和 STM32 TX 增加 2 ms 帧间 guard，覆盖 STM32 `DMA_NORMAL + ReceiveToIdle`
  在 IDLE 回调后由任务重新 arm DMA 的接收空窗。
- UART TX、session 表、pending 表、cluster VFS route/fd 表均增加 mutex。
- route/fd 增加 generation，boot 变化、地址复用或离线后旧操作返回 `stale_boot`。

## PC 验证

`pwos_host_session_test` 覆盖：

- mcu1/mcu2 同时请求，pending peak 为 2，wire tag 不重复。
- src 地址错误或 tag 错误的响应不会唤醒请求。
- deadline 后 pending 槽被释放。
- boot_id 变化会取消旧请求并唤醒等待任务。

`pwos_host_api_test` 覆盖：

- route 同步和稳定 `mcuN` 命名。
- attach/open/read/clunk 数据面。
- boot 变化重新 attach、离线 no-route、deadline 不伪装 EAGAIN。

## 上板检查

P4 日志应持续出现：

```text
mini9p mcu1 addr=1 /sys/health=ok
mini9p mcu2 addr=2 /sys/health=ok
stats ... pending=<delivered>/<unmatched> peak=<n> parse_err=0 tx_err=0
```

正常自动探测时 `unmatched` 应保持 0，单探测 worker 下 `peak=1` 是正常值；
PC 并发测试必须达到 `peak=2`。
节点复位时允许出现 `stale_boot`，但 mcu1 健康检查不能被 mcu2 的超时阻塞。

## 下一步

进入 M7：异步 WebShell/LAN、每客户端命令队列和 `/sys` 可观测性。WebShell
不得直接读取 UART，只能调用 `cluster_vfs`。
