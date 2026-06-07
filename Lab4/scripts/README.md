# Lab4 测试脚本说明

本目录脚本与 `docs/report.md` 各节一一对应。所有结果（CSV / 日志 / 模型输出）统一写入 `scripts/results/`（脚本自动创建）。

## 运行前准备

- 模型已在 `llama.cpp/models/qwen2.5-7b-instruct-q4_k_m.gguf`；二进制在 `llama.cpp/build/bin/`。
- 在 **Lab4 根目录**下运行（脚本内部自行解析路径，无需手动 `cd`）。
- 脚本是 bash `.sh`，可直接执行；如权限丢失，先运行：
  ```bash
  chmod +x scripts/*.sh
  ```
- 所有脚本也支持 `bash scripts/<name>.sh ...` 方式运行。

## 文件清单与对应章节

| 脚本 | 对应报告章节 | 作用 |
| ---- | ------------ | ---- |
| `common.sh` | — | 公共路径与辅助函数（被其它脚本 source，不单独运行） |
| `23_metrics.sh` | 2.3 | 测量加载时间 / TTFT / prefill / decode / 峰值内存 |
| `24_tune.sh` | 2.4 | 参数单变量扫描：threads / batch / ctx / mmap / ngl |
| `prompts_5.json` | 2.5 | 5 个评测 prompt（中文问答/摘要/代码/推理/课程） |
| `25_quality.sh` | 2.5 | 跑 5 个 prompt 并落盘，供人工打分；可换配置对比 |
| `26_build_rpc.sh` | 2.6 | 用 `-DGGML_RPC=ON -DGGML_RPC_RDMA=OFF -DGGML_CCACHE=OFF` 重新构建到 `build-rpc/` |
| `26_rpc_worker.sh` | 2.6 | 【从机】启动 `rpc-server` |
| `27_rpc_compare.sh` | 2.6 / 2.7 | 【主机】单机 vs RPC 性能对比 |
| `28_quant_compare.sh` | 2.8 | 不同量化 Q4/Q5/Q8 体积/吞吐/内存对比 |
| `prompts.json` | 3.3 | Ray 批量推理用的 22 个 prompt |
| `ray_start_servers.sh` | 3.2 | 启动 `llama-server` HTTP 服务（每节点/每端口一个） |
| `ray_dispatch.sh` | 3.4 / 3.5 | Ray 分发请求 + 采集指标 + 串行/单机并行/多机并行对比 |
| `ray_pressure.sh` | 3.7 | Ray 并发压力测试，输出平均延迟 / P95 / 吞吐 / 失败数 |
| `ray_retry.sh` | 3.7 | Ray 失败重试测试，故障节点失败后转发到健康节点 |

---

## 用法

### 2.3 核心指标
```bash
./scripts/23_metrics.sh
# 可调线程/生成长度：
./scripts/23_metrics.sh --threads 8 --n-predict 128
```
输出：`results/23_bench_*.md`（pp512/tg128 吞吐表）、`results/23_summary_*.csv`（加载时间/TTFT/峰值内存）。

### 2.4 参数调优
```bash
./scripts/24_tune.sh --test threads   # 线程扫描 -> decode
./scripts/24_tune.sh --test batch     # 批大小 -> prefill
./scripts/24_tune.sh --test ctx       # 上下文 -> 峰值内存
./scripts/24_tune.sh --test mmap      # mmap 开关 -> 加载时间
./scripts/24_tune.sh --test ngl       # GPU offload（需 CUDA 构建）
./scripts/24_tune.sh --test all       # 除 ngl 外全部
```

### 2.5 输出质量评估
```bash
# 配置A：Q4_K_M, temp=0.7
./scripts/25_quality.sh --tag A --temp 0.7
# 配置B：换更高位量化（先下载 q8_0 到 models）或改 temperature
./scripts/25_quality.sh --tag B --temp 0.7 --model ./llama.cpp/models/qwen2.5-7b-instruct-q8_0.gguf
```
每个 prompt 的输出落到 `results/25_quality_<Tag>_*/pN_*.txt`，人工按 1~5 分打分填表。

### 2.6 / 2.7 RPC 分布式
```bash
# 1) 两台机器都构建（或至少从机）
./scripts/26_build_rpc.sh
# 2) 从机启动 worker（记下打印的局域网 IP）
./scripts/26_rpc_worker.sh --port 50052
# 3) 主机对比单机 vs RPC
./scripts/27_rpc_compare.sh --rpc 192.168.1.20:50052
```
注意：模型文件要放到主机的 `llama.cpp/models/`；RPC worker 只需要 `rpc-server`。脚本默认关闭 RDMA，普通局域网实验走 TCP 更稳；确需 RDMA 可用 `GGML_RPC_RDMA=ON ./scripts/26_build_rpc.sh`。

### 2.8 量化对比（选做）
```bash
# 先下载其它量化：
modelscope download --model Qwen/Qwen2.5-7B-Instruct-GGUF qwen2.5-7b-instruct-q5_k_m.gguf --local_dir ./llama.cpp/models
modelscope download --model Qwen/Qwen2.5-7B-Instruct-GGUF qwen2.5-7b-instruct-q8_0.gguf --local_dir ./llama.cpp/models
./scripts/28_quant_compare.sh
```

### 三、Ray 批量调度
```bash
# 1) 起服务（同机模拟两节点：两个终端，不同端口）
./scripts/ray_start_servers.sh --port 8080
./scripts/ray_start_servers.sh --port 8081

# 2) 组 Ray 集群
ray start --head --port=6379 --dashboard-host=0.0.0.0
# 从机：ray start --address='192.168.1.10:6379'

# 3) 分发并对比三种方式
pip install "ray[default]" requests
./scripts/ray_dispatch.sh --servers http://127.0.0.1:8080 http://127.0.0.1:8081 --mode all
```
单机自测可加 `--ray-address local`。输出：`results/ray_detail_*.csv`（逐请求 start/end/dur/out_len）、`results/ray_summary_*.csv`（串行/单机并行/多机并行的总耗时/平均延迟/吞吐）。

### Ray 加分项
```bash
# 并发压力测试
./scripts/ray_pressure.sh --servers http://127.0.0.1:18080 --limit 6 --n-predict 4 --concurrency 1,2,4 --ray-address local

# 失败重试测试：18081 模拟故障节点，18080 为健康节点
./scripts/ray_retry.sh --servers http://127.0.0.1:18081 http://127.0.0.1:18080 --limit 4 --n-predict 1 --timeout 20 --ray-address local
```

---

## 说明

- llama-bench 默认 `-r 5` 重复 5 次取均值±std，运行较慢属正常。
- `-st`（--single-turn）让 llama-cli 单轮非交互运行后自动退出，便于脚本采集计时；本机实测 `-no-cnv` 会卡住等待输入，故统一改用 `-st`。
- 峰值内存通过轮询进程 RSS 采集（见 `common.sh` 的 `invoke_with_peak_memory`）。
- 含中文的 prompt 统一写成 UTF-8 临时文件再用 `-f` 传入，25/27 已采用此方式。
