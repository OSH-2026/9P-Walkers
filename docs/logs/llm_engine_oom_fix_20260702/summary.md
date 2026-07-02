# llm_engine.cpp 内存溢出修复 + main.cpp 安全加固

> 日期: 2026-07-02  
> 影响范围:
> - `pwos-master-esp32s3/inference/llm_engine.cpp`
> - `pwos-master-esp32p4/inference/llm_engine.cpp`
> - `pwos-master-esp32s3/main/main.cpp`
> - `pwos-master-esp32s3/sdkconfig.defaults`

---

## Part A: llm_engine.cpp 修复清单（共 7 项）

### Bug #1 [致命] EOS 回退导致 pos 为负 → 堆破坏

**根因**: `generate()` 中模型输出 EOS (token=1) 时执行 `pos--; continue;`，若 pos 已为 0，pos 变为 -1。下一轮 `forward()` 中 `key_cache + pos * kv_dim` 越界写入，破坏堆元数据。

**修复**: pos > 0 才回退，否则 fallback 到安全 token 0 (`<unk>`)。

```cpp
// 修改前
if (next == 1) { pos--; continue; }

// 修改后
if (next == 1) {
    if (pos > 0) { pos--; continue; }
    next = 0;  // fallback to <unk>
}
```

---

### Bug #2 [致命] safe_alloc 大块分配错误回退到内部 SRAM

**根因**: PSRAM 满时，KV cache 等大块分配（~640 KB）回退到内部 SRAM（ESP32-S3 仅 ~512 KB），必定失败触发 `abort()`。

**修复**: 超过 32 KB 的分配不再尝试回退到内部 SRAM，直接失败并打印 PSRAM/内部空闲量诊断。

---

### Bug #3 [致命] READY_BIT 与 FORWARD_TASK_2 位值冲突

**根因**: `#define READY_BIT (1 << 3)` 与 `FORWARD_TASK_2 (1 << 3)` 值相同（均为 bit 3）。

**修复**: 删除未使用的 `READY_BIT` 宏。

---

### Bug #4 [致命] 信号量/事件组无超时 → 死锁

**根因**: 所有 `xSemaphoreTake` 和 `xEventGroupSync` 使用 `portMAX_DELAY`。若 helper 任务（matmul_task / forward_task）因栈溢出等原因卡死，主任务永久阻塞，Task Watchdog 5 秒后不可预测地重启。

**修复**: 添加超时——matmul 同步 2 秒、forward 同步 5 秒，超时后主动 `esp_restart()` 并打印错误原因。

---

### Bug #5 [中等] encode() 中 sprintf 无界写入

**根因**: BPE 合并循环中使用 `sprintf(str_buffer, "%s%s", ...)`，若 tokenizer 文件损坏导致词条超长，会写穿缓冲区。

**修复**: 替换为 `snprintf` 并检查返回值。

---

### Bug #6 [中等] free_transformer 未清理 FreeRTOS 资源

**根因**: 只释放了模型数据和 RunState，未删除 task、semaphore、event group。

**修复**: 添加完整的 FreeRTOS 资源清理（vTaskDelete, vSemaphoreDelete, vEventGroupDelete）。

---

### Bug #7 [中等] Helper 任务栈过小

**根因**: `matmul_task` 和 `forward_task` 栈仅 4096 字节，内部有 softmax 调用链和局部变量。

**修复**: 栈空间增至 6144 字节。

---

## 附加改进

- `safe_alloc` 分配前记录空闲 PSRAM/内部 SRAM 量；大块分配（> 总空闲的一半）打印警告
- `malloc_run_state` 新增 KV cache 大小日志
- `forward()` 中 FFN 逻辑从 `if(xSemaphoreTake)` 嵌套块中移出，确保信号量失败时不会静默跳过整层 FFN

---

## 内存预算（stories260K 模型 @ ESP32-S3）

| 组件 | 估计大小 |
|------|----------|
| 模型权重 | ~1.1 MB |
| KV Cache (key + value) | ~0.64 MB |
| 其他 RunState 缓冲区 | ~0.03 MB |
| Tokenizer 词表 | ~0.02 MB |
| **合计** | **~1.8 MB** |

ESP32-S3 最小 PSRAM 为 2 MB。扣除 WiFi / LWIP / 其他组件后余量紧张，建议优先使用 8 MB PSRAM 模组，或将 `seq_len` 降至 128。

---

## 验证建议

1. 烧录后观察串口日志中 "KV cache 大小" 和 "PSRAM 空闲" 的数值
2. 连续发起多次推理请求，确认不再出现 `Guru Meditation Error` 或 `abort()`
3. 若仍重启，注意日志中最新的 `ESP_LOGE` 消息定位具体 OOM 阶段

---

## Part B: main.cpp 安全加固（共 8 项）

### 风险 #1 [高] `pwos_dist_inference_service_init()` 返回值被 `(void)` 忽略
**修复**: 检查返回值并记录错误日志，但不阻塞后续推理引擎启动。

### 风险 #2 [高] 推理引擎启动失败仅打日志
**修复**: 推理为核心功能，启动失败后 5 秒重启（`esp_restart()`），而非静默进入空闲循环。

### 风险 #3 [中] `print_banner()` 泄露 PSRAM 内存信息
**修复**: 仅输出芯片型号与核心数；PSRAM 总量/空闲量降级为 `ESP_LOGD`（DEBUG 级别，默认不输出）。

### 风险 #4 [中] `app_main` 栈大小仅 3584 字节
**修复**: `sdkconfig.defaults` 新增 `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`。

### 风险 #5 [中] 组件启动无依赖校验
**修复**: 引入 `boot_phase_t` 阶段枚举 + `advance_phase()` 追踪启动进度，超时 30 秒自动重启。

### 风险 #6 [中] 无初始化超时保护
**修复**: `advance_phase()` 内检查全局 `PWOS_BOOT_TIMEOUT_MS`（30 秒），超时 `esp_restart()`。

### 风险 #7 [低] 无启动完成健康检查
**修复**: 增加 "系统就绪" 日志（含启动耗时）；新增 `boot_phase_t` 可在外部诊断时读取。

### 风险 #8 [低] 空闲循环无自检
**修复**: 每 30 秒检查 PSRAM 空闲量；若 < 128 KB 主动重启以防 OOM 连锁故障。

### 增强: 任务看门狗 (TWDT)
**修复**: 系统就绪后将 `app_main` 注册到 ESP-IDF Task Watchdog（超时 5s），空闲循环中每 3 秒 `esp_task_wdt_reset()`。若主循环卡死 > 5s → TWDT 自动重启。此外 TWDT 默认监控两个核心的 idle task，可间接保护 `inference_worker`、`matmul_task`、`forward_task` 等子任务不死锁。

---

## Part C: 野指针/悬空指针审查与修复（共 4 项）

### 修复 #1 [高] `inference_runtime.cpp` — 部分初始化失败产生僵尸 worker task
**问题**: `pwos_inference_runtime_start()` 中 `inference_worker` 创建成功但 `inference_console` 失败时，已运行的 worker 成为无人管理的僵尸任务。
**修复**: 失败时 `vTaskDelete` 已创建的 worker，并完整清理 mutex/queue/output buffer，重置 `initialized` 标志。

### 修复 #2 [中] `dist_inference_service.c` — `snprintf` 返回值用 `int` 累加隐藏截断
**问题**: 两次 `snprintf` 返回 `int`，用 `len += snprintf(...)` 累加后，通过 `(uint16_t)len >= *in_out_len` 检查截断。但第二次 `snprintf` 参数 `*in_out_len - len` 使用上次已截断的 `len` 计算剩余空间，检查不精确。
**修复**: 改为分步计算 `written`（`uint16_t`），第二次调用用 `buf_cap - written`，分别检查每次 `snprintf` 返回值，最后统一判断是否截断。

### 修复 #3 [中] `llm_engine.cpp` — `generate()` prompt_tokens 分配用 `int` 接收 `strlen` 结果
**问题**: `safe_alloc<int>((strlen(prompt) + 3))` 中 `strlen` 返回 `size_t`，加 3 后隐式截断为 `int`（在 ESP32 上 `int` 为 32 位，`size_t` 为 32 位，实际不会截断，但语义不清）。
**修复**: 使用显式 `size_t` 类型计算 `max_tokens`，并增加 1024 上限保护，超长 prompt 直接拒绝而非 crash。

### 修复 #4 [中] `llm_engine.cpp` — `sample_topp()` 缺少 `probindex` 空指针检查
**问题**: `probindex` 由 `build_sampler()` 分配，若 sampler 未正确初始化即被调用（如内存不足），`probindex` 为 NULL 会导致崩溃。
**修复**: 函数入口增加 `if (probindex == NULL || n <= 0) return 0;` 防御检查。
