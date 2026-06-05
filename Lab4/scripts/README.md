# Lab4 测试脚本说明

本目录脚本与 `docs/report.md` 各节一一对应。所有结果（CSV / 日志 / 模型输出）统一写入 `scripts/results/`（脚本自动创建）。

## 运行前准备

- 模型已在 `llama.cpp/models/qwen2.5-7b-instruct-q4_k_m.gguf`；二进制在 `llama.cpp/build/bin/Release/`。
- 在 **Lab4 根目录**下运行（脚本内部自行解析路径，无需手动 cd）。
- PowerShell 执行策略若拦截脚本，先执行：
  ```powershell
  Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
  ```
- 所有 `.ps1` 已存为 UTF-8 BOM，可被 Windows PowerShell 5.1 正确解析中文。

## 文件清单与对应章节

| 脚本                    | 对应报告章节 | 作用 |
| ----------------------- | ------------ | ---- |
| `common.ps1`            | —            | 公共路径与辅助函数（被其它脚本 dot-source，不单独运行） |
| `23_metrics.ps1`        | 2.3          | 测量加载时间 / TTFT / prefill / decode / 峰值内存 |
| `24_tune.ps1`           | 2.4          | 参数单变量扫描：threads / batch / ctx / mmap / ngl |
| `prompts_5.json`        | 2.5          | 5 个评测 prompt（中文问答/摘要/代码/推理/课程） |
| `25_quality.ps1`        | 2.5          | 跑 5 个 prompt 并落盘，供人工打分；可换配置对比 |
| `26_build_rpc.ps1`      | 2.6          | 用 `-DGGML_RPC=ON` 重新构建到 `build-rpc/` |
| `26_rpc_worker.ps1`     | 2.6          | 【从机】启动 `rpc-server` |
| `27_rpc_compare.ps1`    | 2.6 / 2.7    | 【主机】单机 vs RPC 性能对比 |
| `28_quant_compare.ps1`  | 2.8          | 不同量化 Q4/Q5/Q8 体积/吞吐/内存对比 |
| `prompts.json`          | 3.3          | Ray 批量推理用的 22 个 prompt |
| `ray_start_servers.ps1` | 3.2          | 启动 `llama-server` HTTP 服务（每节点/每端口一个） |
| `ray_dispatch.py`       | 3.4 / 3.5    | Ray 分发请求 + 采集指标 + 串行/单机并行/多机并行对比 |

---

## 用法

### 2.3 核心指标
```powershell
pwsh -File .\scripts\23_metrics.ps1
# 可调线程/生成长度：
pwsh -File .\scripts\23_metrics.ps1 -Threads 8 -NPredict 128
```
输出：`results/23_bench_*.md`（pp512/tg128 吞吐表）、`results/23_summary_*.csv`（加载时间/TTFT/峰值内存）。

### 2.4 参数调优
```powershell
pwsh -File .\scripts\24_tune.ps1 -Test threads   # 线程扫描 → decode
pwsh -File .\scripts\24_tune.ps1 -Test batch     # 批大小 → prefill
pwsh -File .\scripts\24_tune.ps1 -Test ctx       # 上下文 → 峰值内存
pwsh -File .\scripts\24_tune.ps1 -Test mmap      # mmap 开关 → 加载时间
pwsh -File .\scripts\24_tune.ps1 -Test ngl       # GPU offload（需 CUDA 构建）
pwsh -File .\scripts\24_tune.ps1 -Test all       # 除 ngl 外全部
```

### 2.5 输出质量评估
```powershell
# 配置A：Q4_K_M, temp=0.7
pwsh -File .\scripts\25_quality.ps1 -Tag A -Temp 0.7
# 配置B：换更高位量化（先下载 q8_0 到 models）或改 temperature
pwsh -File .\scripts\25_quality.ps1 -Tag B -Temp 0.7 -Model .\llama.cpp\models\qwen2.5-7b-instruct-q8_0.gguf
```
每个 prompt 的输出落到 `results/25_quality_<Tag>_*/pN_*.txt`，人工按 1~5 分打分填表。

### 2.6 / 2.7 RPC 分布式
```powershell
# 1) 两台机器都构建（或至少从机）
pwsh -File .\scripts\26_build_rpc.ps1
# 2) 从机启动 worker（记下打印的局域网 IP）
pwsh -File .\scripts\26_rpc_worker.ps1 -Port 50052
# 3) 主机对比单机 vs RPC
pwsh -File .\scripts\27_rpc_compare.ps1 -Rpc 192.168.1.20:50052
```
注意：模型文件要放到 `build-rpc` 能访问到的同路径（脚本默认仍用 `llama.cpp/models/`）。

### 2.8 量化对比（选做）
```powershell
# 先下载其它量化：
modelscope download --model Qwen/Qwen2.5-7B-Instruct-GGUF qwen2.5-7b-instruct-q5_k_m.gguf --local_dir .\llama.cpp\models
modelscope download --model Qwen/Qwen2.5-7B-Instruct-GGUF qwen2.5-7b-instruct-q8_0.gguf   --local_dir .\llama.cpp\models
pwsh -File .\scripts\28_quant_compare.ps1
```

### 三、Ray 批量调度
```powershell
# 1) 起服务（同机模拟两节点：两个终端，不同端口）
pwsh -File .\scripts\ray_start_servers.ps1 -Port 8080
pwsh -File .\scripts\ray_start_servers.ps1 -Port 8081

# 2) 组 Ray 集群
ray start --head --port=6379 --dashboard-host=0.0.0.0
# 从机：ray start --address='192.168.1.10:6379'

# 3) 分发并对比三种方式
pip install "ray[default]" requests
python scripts\ray_dispatch.py --servers http://127.0.0.1:8080 http://127.0.0.1:8081 --mode all
```
单机自测可加 `--ray-address local`。输出：`results/ray_detail_*.csv`（逐请求 start/end/dur/out_len）、`results/ray_summary_*.csv`（串行/单机并行/多机并行的总耗时/平均延迟/吞吐）。

---

## 说明
- llama-bench 默认 `-r 5` 重复 5 次取均值±std，运行较慢属正常。
- `-st`（--single-turn）让 llama-cli 单轮非交互运行后自动退出，便于脚本采集计时；本机实测 `-no-cnv` 会卡住等待输入，故统一改用 `-st`。
- 峰值内存通过轮询进程 `WorkingSet64` 采集（见 `common.ps1` 的 `Invoke-WithPeakMemory`）。
- **中文编码**：Windows 上有两处坑，已在 `common.ps1` 统一处理——
  ① 读取 llama-cli 输出时强制按 UTF-8 解码（`ProcessStartInfo.StandardOutputEncoding`），否则会被系统 GBK 误解码成乱码；
  ② 含中文的 prompt 不能用命令行 `-p` 传（argv 会被转成 GBK 而乱码），改用 `New-PromptFile` 写成 UTF-8 文件再用 `-f` 传入。25/27 已采用此方式。
