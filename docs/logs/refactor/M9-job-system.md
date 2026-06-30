# M9 Job System

日期：2026-06-30  
基线：`6869a57` 之后的工作树  
状态：完成，两节点生命周期、结果、取消与 health 并行性已上板通过

## 范围

- 新增 `DATA_JOB` 固定头协议，支持 caps、submit、status、result、cancel；请求 ID
  复用 `session_manager` 的全局 wire tag，pending 按 `(data_type, src, tag)` 隔离。
- P4 `job_manager` 使用 16 个静态槽，保留原始输入以支持 LOST 后显式 retry；节点
  重启、事务 deadline 或远端 job 消失时标记 LOST。LOST job 禁止继续查询，避免新
  boot 上远端 ID 复用导致旧任务被错误复活。
- STM32 `compute_worker` 使用 4 个静态槽、320 字节输入和 320 字节结果缓冲；每次
  step 只推进一个字节、向量元素、矩阵元素或像素。
- compute worker 是独立低优先级 FreeRTOS task。`service_task` 只做短时协议解析和
  job 控制，不等待计算完成，因此不阻塞 hello、lease、route、mini9P 和 RPC。
- 第一批 kernel：FNV-1a `hash`、`vector_add`、`matmul`、`mandelbrot`。
- 从机新增只读 `/compute/caps`、`/compute/load`、`/compute/jobs`；P4 新增
  `/host/sys/jobs`。任务控制使用 DATA_JOB，命令入口是 WebShell `job ...`。
- 本阶段结果随单个 DATA_JOB 响应返回，上限 320 字节。更大的输入/结果与分片调度
  留给后续 `DATA_BULK`，不在 M9 inline 实现中伪装分块。

## 已执行验证

- Job codec：大小端、retag、所有 kind/state/status、畸形帧拒绝。
- Compute worker：四个 kernel 精确结果、progress、cancel、静态槽复用。
- Job service：caps、submit/status/result/cancel，matmul 期望
  `[19,22;43,50]`，mandelbrot 取消。
- Host session/job manager：typed pending、生命周期、deadline→LOST、节点重启
  LOST、retry 创建新 host job、caps/result 解码。
- WebShell command：job 回调、caps/submit/list/status/result 路径。
- local VFS：根目录包含 `sys/`、`compute/`，compute 下三个观测节点可枚举。
- 全部 PC CTest 通过；补齐 mesh2 与 host coordinator 的 `add_test` 后也由 CTest
  实际执行通过。
- F407 Debug：RAM 75632 B / 128 KiB（57.70%），FLASH 102964 B / 512 KiB
  （19.64%）。
- ESP32-P4：固件 `0x8c970`，app 分区剩余 `0x73690`（45%）。
- `git diff --check` 通过。

## 两节点上板验收

拓扑：`P4 -> MCU1 USART2 -> MCU1 USART1 -> MCU2 USART1`。确认两节点 health 与
RPC ping 稳定后，在 WebShell 执行：

```text
cat /mcu1/compute/caps
cat /mcu2/compute/load
job caps mcu1
job caps mcu2

job submit mcu1 hash hello-mcu1
job status <id>
job result <id>

job submit mcu1 vector_add 8
job result <id>

job submit mcu2 matmul
job status <id>
job result <id>

job submit mcu2 mandelbrot 16 16 80
job status <id>
job result <id>

job list
cat /host/sys/jobs
cat /mcu1/compute/jobs
cat /mcu2/compute/jobs
```

判定标准：

1. hash 返回 32 位 FNV-1a；vector_add 默认每项为 8；matmul 返回
   `matrix=2x2 [19,22;43,50]`；mandelbrot 返回 16×16 字符图。
2. 快速 status 可见 queued/running，完成后为 done、progress=100.0%；过早 result
   明确返回 `result not ready`，不能返回残缺数据。
3. 为可靠测试 cancel，可连续提交多个 16×16 mandelbrot，再取消最后一个 queued
   job；其状态应为 cancelled，其他任务继续完成。
4. mcu2 job 运行时断开或复位 mcu2，再查询该 job 应变为 LOST；节点恢复后执行
   `job retry <id>` 产生新的 host job ID，并可正常完成。
5. 计算和 result 输出期间，P4 周期 health、`rpc mcu1/mcu2 system.ping`、hello、
   lease 与 route 均继续正常；`job_malformed`、link parse error、TX error 不增长。

以上通过后，M9 完成并进入 M10 主机间平面。

2026-06-30，用户确认上述两节点验收无问题。M9 结束，进入 M10。
