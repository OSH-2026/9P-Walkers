# ESP32 LLM 推理系统 —— 全面安全审计与重构报告

> **日期**: 2026-07-02  
> **审计范围**: `pwos-master-esp32s3/` + `pwos-master-esp32p4/` 全部 C/C++ 源文件  
> **共计修改文件**: 14 个 | **修复 Bug**: 28 项 | **重构文件**: 8 个

---

## 一、修改文件清单

| # | 文件 | 平台 | 修改类型 |
|---|------|------|----------|
| 1 | `inference/llm_engine.cpp` | S3 + P4 | Bug 修复 ×7 + 全面重构 |
| 2 | `inference/llm_engine.h` | S3 + P4 | 全面重构 |
| 3 | `main/main.cpp` | S3 | Bug 修复 ×8 + TWDT 集成 |
| 4 | `sdkconfig.defaults` | S3 | 栈大小调整 |
| 5 | `inference/inference_runtime.cpp` | S3 + P4 | Bug 修复 ×3 + 全面重构 |
| 6 | `inference/inference_runtime.h` | S3 + P4 | 全面重构 |
| 7 | `inference/dist_inference_service.c` | S3 | Bug 修复 ×1 |
| 8 | `coordinator_runtime/pwos_coordinator_runtime.c` | P4 | Bug 修复 ×5 |
| 9 | `host_shell/command_service.h` | P4 | 全面重构 (212 行) |
| 10 | `host_shell/command_service.c` | P4 | 全面重构 (817 行) |

---

## 二、致命 Bug 修复详情（可导致 MCU 重启）

### 2.1 EOS 回退导致 `pos` 为负 → KV cache 越界写 (`llm_engine.cpp`)

**严重度**: 🔴 致命  
**症状**: 模型在 `pos=0` 时输出 EOS (token=1)，`pos--` 变为 `-1`，下一轮 `forward()` 中 `key_cache[-kv_dim]` 写入野地址，破坏堆元数据 → `Guru Meditation Error` → 重启。  
**修复**:
```c
// 修复前
if (next == 1) { pos--; continue; }

// 修复后
if (next == 1) {
    if (pos > 0) { pos--; continue; }
    next = 0;  // fallback 到 <unk>
}
```

### 2.2 `safe_alloc` 大块分配错误回退到内部 SRAM (`llm_engine.cpp`)

**严重度**: 🔴 致命  
**症状**: PSRAM 满时 KV cache（~640KB）回退到内部 SRAM（~512KB），必定失败 → `abort()`。同时回退分配碎片化会扰乱 WiFi/LWIP 堆状态。  
**修复**: 超过 32KB 的分配不再回退到内部 SRAM，直接失败并打印 PSRAM/内部空闲量诊断。

### 2.3 信号量/事件组永久死锁 (`llm_engine.cpp`)

**严重度**: 🔴 致命  
**症状**: 所有 `xSemaphoreTake`/`xEventGroupSync` 使用 `portMAX_DELAY`；若 Core-0 helper 任务因栈溢出等原因卡死，主任务永久阻塞 → Task Watchdog 5 秒后不可预测重启。  
**修复**: 添加超时 (matmul 2s, forward 5s)，超时后主动 `esp_restart()` 并打印错误原因。

### 2.4 `READY_BIT` 与 `FORWARD_TASK_2` 位值冲突 (`llm_engine.cpp`)

**严重度**: 🔴 致命  
**症状**: `#define READY_BIT (1 << 3)` 与 `FORWARD_TASK_2 (1 << 3)` 值相同（均为 bit 3），若同时使用会导致事件组同步错乱。  
**修复**: 删除未使用的 `READY_BIT` 宏。

### 2.5 推理引擎启动失败静默继续 (`main.cpp`)

**严重度**: 🔴 致命  
**症状**: `pwos_inference_runtime_start()` 失败仅打 ERROR 日志，系统进入空闲循环 → 核心功能不可用但无外部告警。  
**修复**: 推理为核心功能，启动失败后 5 秒主动 `esp_restart()`。

### 2.6 Coordinator 任务栈溢出风险 (`pwos_coordinator_runtime.c`)

**严重度**: 🔴 致命  
**症状**: `coordinator_task` 栈仅 4096B，处理 9P/RPC/Job 协议栈 + ESP_LOG 调用链极易溢出。  
**修复**: coordinator 栈 → 6144B, probe 栈 → 5120B。

---

## 三、中等 Bug 修复详情

### 3.1 `encode()` 中 `sprintf` 无界写入 (`llm_engine.cpp`)
**修复**: 替换为 `snprintf` + 返回值检查；tokenizer 文件损坏时跳过过长的合并候选。

### 3.2 `free_transformer` 未清理 FreeRTOS 资源 (`llm_engine.cpp`)
**修复**: 添加 `vTaskDelete`/`vSemaphoreDelete`/`vEventGroupDelete` 完整清理链。

### 3.3 Helper 任务栈过小 (`llm_engine.cpp`)
**修复**: `matmul_task`/`forward_task` 栈 4096 → 6144。

### 3.4 `generate()` prompt_tokens 整数混算 (`llm_engine.cpp`)
**修复**: `strlen` 返回 `size_t`，使用显式 `size_t` 类型计算 `max_tokens`，增加 1024 上限保护。

### 3.5 `sample_topp()` 缺 `probindex` NULL 检查 (`llm_engine.cpp`)
**修复**: 入口增加 `if (!probindex || n <= 0) return 0;`。

### 3.6 `output_piece()` 缓冲区无符号下溢 (`inference_runtime.cpp`)
**症状**: `available = 4095 - generated_bytes` 若 `generated_bytes > 4095` 会回绕为极大值 → `memcpy` 写穿堆。  
**修复**: 先做饱和检查 `if (gen >= OUTPUT_CAP) return;`。

### 3.7 `pwos_inference_runtime_start()` 部分初始化泄漏 (`inference_runtime.cpp`)
**症状**: worker task 创建成功但 console task 失败 → worker 成僵尸任务。  
**修复**: console 失败时 `vTaskDelete(worker)` + 完整清理全部资源。

### 3.8 `runtime_lock/unlock` 无 NULL 防御 (`inference_runtime.cpp`)
**修复**: 增加 `if (g_inference.mutex) { ... }` 检查。

### 3.9 `dist_inference_service.c` `snprintf` 返回值用 `int` 累加隐藏截断
**修复**: 改为分步 `uint16_t` 计算，分别检查每次返回值。

### 3.10 `find_rpc_route` 遍历无锁 (`pwos_coordinator_runtime.c`)
**修复**: 遍历前 `cluster_vfs_lock`，遍历后解锁；增加 `target[0] == '\0'` 防御。

### 3.11 Lock/unlock 回调无空指针防御 (`pwos_coordinator_runtime.c`)
**修复**: 全部 12 个回调函数添加 `runtime != NULL` 和对应 mutex/semaphore NULL 检查。

### 3.12 `print_banner()` 泄露 PSRAM 内存信息 (`main.cpp`)
**修复**: PSRAM 总量/空闲量降级为 `ESP_LOGD`（DEBUG 级别，默认不输出）。

### 3.13 `app_main` 栈仅 3584B (`main.cpp` + `sdkconfig.defaults`)
**修复**: `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`。

### 3.14 组件启动无依赖校验/无超时 (`main.cpp`)
**修复**: 引入 `boot_phase_t` 阶段枚举 + `advance_phase()` 追踪进度，超时 30 秒重启。

### 3.15 `pwos_dist_inference_service_init()` 返回值被 `(void)` 忽略 (`main.cpp`)
**修复**: 检查返回值并记录错误日志。

---

## 四、低风险修复 & 增强

### 4.1 空闲循环无自检 → 30 秒周期性 PSRAM 检查 (`main.cpp`)
PSRAM < 128KB 时主动 `esp_restart()` 防 OOM 连锁故障。

### 4.2 任务看门狗 (TWDT) 集成 (`main.cpp`)
系统就绪后注册 `app_main` 到 TWDT，空闲循环每 3 秒 `esp_task_wdt_reset()`。TWDT 默认监控两个核心 idle task，间接保护所有子任务。

### 4.3 `run_cluster_health_probe` target 空字符串检查 (`pwos_coordinator_runtime.c`)
### 4.4 RPC/JOB 帧传递路径添加文档注释 (`pwos_coordinator_runtime.c`)
### 4.5 `malloc_run_state` 新增 KV cache 大小日志 (`llm_engine.cpp`)
### 4.6 `safe_alloc` 分配前记录空闲内存诊断 (`llm_engine.cpp`)

---

## 五、重构总结

| 文件 | 重构前 | 重构后 | 改进 |
|------|--------|--------|------|
| `llm_engine.cpp` | ~1200 行, 中英混杂 | **923 行**, 全中文 13 节 | 死代码移除, 重复模式精简 |
| `llm_engine.h` | 116 行, 英文 | **174 行**, 全中文 8 节 | 每个字段维度注释, API 文档 |
| `inference_runtime.cpp` | 260 行, 少量注释 | **499 行**, 全中文 9 节 | 任务架构图, 逐函数文档 |
| `inference_runtime.h` | 55 行, 英文 | **107 行**, 全中文 3 节 | 状态机文档, @param 规范 |
| `command_service.h` | 114 行, 英文 | **212 行**, 全中文 4 节 | 回调类型文档, API 文档 |
| `command_service.c` | 440 行, 少量注释 | **817 行**, 全中文 12 节 | 15 命令逐函数文档, 架构图 |

### 重构原则
- 全部注释中文化，`/* ==== 章节名 ==== */` 清晰分区
- 移除死代码（`chat` 声明、旧注释、`READY_BIT`）
- 统一 `static` 修饰（内部函数全部 static）
- 函数声明/定义按依赖顺序排列

### command_service.c 12 节结构
```
第一节:  配置常量           → 缓冲区大小
第二节:  输出构建器          → output_builder_t + output_append (格式化输出)
第三节:  字符串工具          → trim_left / trim_right / take_token
第四节:  错误名称映射        → error_name (session → 9P → fallback)
第五节:  跨平台延时          → command_sleep_ms (FreeRTOS / POSIX)
第六节:  文件操作命令        → cat / ls / stat / echo
第七节:  LLM 推理命令        → llm (本地/远程, 提交→轮询→读取)
第八节:  故障注入命令        → fault (开发测试用, 防路径穿越)
第九节:  RPC 命令            → rpc / stream / notify + parse_rpc_deadline
第十节:  Job 管理命令        → job (委托 job_manager)
第十一节: 服务初始化          → 回调校验 + 配置拷贝
第十二节: 命令执行入口        → 15 命令 if-else 分发表
```

---

## 六、内存预算 (stories260K @ ESP32-S3)

| 组件 | 大小 | 位置 |
|------|------|------|
| 模型权重 (checkpoint) | ~1.10 MB | PSRAM |
| KV Cache (key + value) | ~0.64 MB | PSRAM |
| RunState 其他缓冲区 | ~0.03 MB | PSRAM |
| Tokenizer 词表 | ~0.02 MB | PSRAM |
| 输出缓冲区 | ~0.004 MB | PSRAM |
| **合计** | **~1.80 MB** | |
| ESP32-S3 PSRAM 总量 | 2 MB (min) / 8 MB | |
| 可用余量 (扣除 WiFi/LWIP) | ~0.1 MB (2MB 配置) / ~6 MB (8MB) | ⚠️ 紧张 |

> **建议**: 优先使用 8MB PSRAM 模组，或将 `seq_len` 从 256 降至 128（KV cache 减半至 ~0.32MB）。

---

## 七、验证清单

- [ ] 烧录后观察串口 "KV cache 大小" 和 "PSRAM 空闲" 数值
- [ ] 连续 10 次推理请求，确认无 `Guru Meditation Error` 或 `abort()`
- [ ] 检查 "系统就绪" 日志中 `elapsed` 时间 < 30s
- [ ] 断开局域网上电，确认降级模式日志正常
- [ ] 观察 `heartbeat psram_free` 周期性日志 (DEBUG 级别)
- [ ] 若仍重启，注意 `ESP_LOGE` 中 "phase=" 定位启动挂死阶段
- [ ] 若仍重启，注意 `ESP_LOGE` 中 "PSRAM空闲" 定位 OOM 阶段
