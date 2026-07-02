# llm_engine.cpp 内存溢出修复记录

> 日期: 2026-07-02  
> 影响范围: `pwos-master-esp32s3/inference/llm_engine.cpp`, `pwos-master-esp32p4/inference/llm_engine.cpp`  
> 问题: ESP32-S3 DevKit 上运行 LLM 推理时频繁内存溢出导致单片机重启

---

## 修复清单（共 7 项）

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
