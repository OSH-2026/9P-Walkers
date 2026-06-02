# OSH LAB4 report

## 一、实验环境

| 项目 | 内容 |
|------|------|
| CPU | Intel Core i9-14900HX（24 核：8 P-core + 16 E-core，共 32 逻辑线程） |
| 内存 | 32 GB（约 31.7 GB 可用） |
| GPU | NVIDIA GeForce RTX 4060 Laptop（8 GB 显存） + Intel UHD Graphics（核显） |
| 操作系统 | Windows 11 Home China（10.0.26200） |
| llama.cpp 版本 | build 9478（commit `60130d18f`），MSVC 19.51 x64 |
| 构建方式 | CMake + MSVC，`-B build`，`--config Release`（CPU 后端） |
| 二进制位置 | `llama.cpp\build\bin\Release\`（llama-cli.exe、llama-bench.exe、llama-server.exe 等） |

> RPC 与 GPU offload 任务需要重新构建（见第六节、第四节优化部分）。

---

## 二、主线部分：llama.cpp

### 2.1 测试性能指标列表

下面列出 6 个 LLM 部署性能指标，并说明选取理由。

|  | 指标 | 定义 / 测量方式 | 选取理由（合理性） |
|---|------|----------------|--------------------|
| 1 | **加载时间 (Load time)** | 从启动进程到模型权重载入完成、可开始推理的时间。llama.cpp 日志中的 `load time`。 | 反映模型从磁盘读入并初始化的开销，直接影响服务冷启动体验，与 `--no-mmap`、磁盘速度、模型大小强相关。 |
| 2 | **首 Token 延迟 (TTFT, Time To First Token)** | 从输入 prompt 到生成第一个 token 的时间，等价于 prompt 预填充 (prefill) 时间。 | 决定交互式场景的"响应快不快"，主要受 prompt 长度、`--batch-size`、线程数影响，是体验类核心指标。 |
| 3 | **输出速度 (Decode throughput)** | 生成阶段每秒产出 token 数 (tokens/s)，对应 llama-bench 的 `tg`（text generation）。 | 衡量持续生成的吞吐能力，是最常用的性能指标，直接体现部署配置的优劣。 |
| 4 | **预填充速度 (Prefill throughput)** | 处理输入 prompt 阶段每秒 token 数，对应 llama-bench 的 `pp`（prompt processing）。 | 与 decode 分开衡量，能区分"读得快"和"写得快"，长 prompt 场景下尤为重要。 |
| 5 | **内存占用 (Memory footprint)** | 推理过程中进程的峰值物理内存 (RSS) / 显存占用。 | 决定一台机器能否装下模型、能并发多少请求，是资源约束类的关键指标，与量化格式、`--ctx-size` 相关。 |
| 6 | **输出质量 (Output quality)** | 用 perplexity（困惑度）或人工打分评估生成内容的正确性 / 流畅性。 | 性能不能脱离质量单独看；量化越激进速度越快但质量可能下降，需要一并评估以判断配置是否"可用"。 |

**合理性**：指标 1 覆盖"启动开销"，2/3/4 覆盖"运行时速度"（分别是体验、生成、预填充三个维度），5 覆盖"资源约束"，6 覆盖"效果"。四个维度共同刻画了一个部署方案是否"又快、又省、又好"，避免只看单一吞吐而误判。

### 2.2 GGUF 量化模型单机部署

**选型说明**：本组选用 **Qwen2.5-7B-Instruct**，量化格式 **Q4_K_M**。
理由：7B 规模在 32GB 内存 / 8GB 显存上可纯 CPU 跑、也可部分 GPU offload；Qwen 系列中文能力强，契合后面中文问答 / 课程问答的评估需求；Q4_K_M 在体积、速度、质量之间平衡较好。

| 项目 | 内容 |
|------|------|
| 模型名称 | Qwen2.5-7B-Instruct |
| 参数规模 | 7B |
| 量化格式 | Q4_K_M |
| 操作系统 | Windows 11 |
| 下载来源 | ModelScope |
| 部署方式 | 单机 CPU 推理（llama-cli） |

**下载命令**：

```powershell
modelscope download --model Qwen/Qwen2.5-7B-Instruct-GGUF qwen2.5-7b-instruct-q4_k_m.gguf --local_dir .\llama.cpp\models
```

**运行命令**：

```powershell
.\llama.cpp\build\bin\Release\llama-cli.exe -m .\llama.cpp\models\qwen2.5-7b-instruct-q4_k_m.gguf -p "What is a large language model?" -n 128 --threads 8
```

**推理结果**：

![2.2](/img/2.2.png)

### 2.3 测试任务设计与指标测量

**测试任务设计**：使用固定 prompt 长度与生成长度，用 `llama-bench` 测量 prefill / decode 吞吐，用 `llama-cli` 的统计行测量加载时间与首 token 延迟，用任务管理器 / 脚本记录峰值内存。

从指标列表中选取 **≥3 个**：①输出速度(decode)、②预填充速度(prefill)、③加载时间。

**测量命令**：

```powershell
# llama-bench：默认会跑 pp512（prefill）和 tg128（decode）
.\llama.cpp\build\bin\Release\llama-bench.exe -m .\llama.cpp\models\qwen2.5-7b-instruct-q4_k_m.gguf
```

**测量结果**：

```bash
(base) PS D:\Course_Repositories\operating-sys\9P-Walkers\Lab4> .\llama.cpp\build\bin\Release\llama-bench.exe -m .\llama.cpp\models\qwen2.5-7b-instruct-q4_k_m.gguf
| model                          |       size |     params | backend    | threads |            test |                  t/s |
| ------------------------------ | ---------: | ---------: | ---------- | ------: | --------------: | -------------------: |
| qwen2 7B Q4_K - Medium         |   4.36 GiB |     7.62 B | CPU        |      24 |           pp512 |         83.93 ± 3.13 |
| qwen2 7B Q4_K - Medium         |   4.36 GiB |     7.62 B | CPU        |      24 |           tg128 |         13.96 ± 0.41 |

build: 60130d18f (9478)
```

| 指标 | 测量值 | 来源 |
|------|--------|------|
| 加载时间 | 【待填写】 ms | llama-cli `load time` |
| 首 Token 延迟 (TTFT) | 【待填写】 ms | llama-cli `prompt eval time` |
| Prefill 吞吐 (pp512) | 【待填写】 t/s | llama-bench |
| Decode 吞吐 (tg128) | 【待填写】 t/s | llama-bench |
| 峰值内存 | 【待填写】 MB | 任务管理器 / 脚本 |



### 2.4 基于部署参数的分析、测试与优化

以 llama.cpp 配置参数为主进行调优。固定模型与 prompt，单独改变一个参数观察影响。

**(1) 线程数 `--threads`**（本机物理大核 8，逻辑 32，需找到最优点）

```powershell
.\llama.cpp\build\bin\Release\llama-bench.exe -m <model> -t 4,8,16,24,32
```

| --threads | 4 | 8 | 16 | 24 | 32 |
|-----------|---|---|----|----|----|
| decode (t/s) | 【】 | 【】 | 【】 | 【】 | 【】 |

**(2) 批大小 `--batch-size` / `-b`**

```powershell
.\llama.cpp\build\bin\Release\llama-bench.exe -m <model> -b 128,256,512,1024
```

| -b | 128 | 256 | 512 | 1024 |
|----|-----|-----|-----|------|
| prefill (t/s) | 【】 | 【】 | 【】 | 【】 |

**(3) 上下文长度 `--ctx-size` / `-c`**：观察对内存占用的影响。【待填写表格】

**(4) 内存映射 `--no-mmap`**：对比开启 / 关闭对加载时间的影响。【待填写表格】

**(5) GPU offload `--n-gpu-layers`（可选，需 CUDA 构建）**：
本机有 RTX 4060（8GB）。若重新用 `-DGGML_CUDA=ON` 构建，可测试 offload 层数对 decode 吞吐的影响。

```powershell
.\llama.cpp\build\bin\Release\llama-bench.exe -m <model> -ngl 0,10,20,33
```

| -ngl | 0 (纯CPU) | 10 | 20 | 33 (全部) |
|------|-----------|----|----|-----------|
| decode (t/s) | 【】 | 【】 | 【】 | 【】 |

**优化结论**：【待填写：找到的最优参数组合，以及相对默认配置的提升百分比，并解释原因（如 E-core 拖慢、显存带宽优势等）】

### 2.5 五个 Prompt 的输出质量评估

设计 5 个 prompt，覆盖 **中文问答 / 摘要 / 代码解释 / 推理题 / 课程相关问题**（≥3 类，此处覆盖全部 5 类）。

| # | 类别 | Prompt |
|---|------|--------|
| 1 | 中文问答 | 请解释什么是"虚拟内存"，并举一个操作系统中的实际例子。 |
| 2 | 摘要 | 请用不超过 50 字概括下面这段话：……（粘贴一段 200 字左右的技术文字） |
| 3 | 代码解释 | 解释这段 C 代码的功能，并指出潜在 bug：`int *p; *p = 10;` |
| 4 | 推理题 | 一个房间里有 3 个开关，对应房间外的 3 盏灯，每个开关控制一盏。你只能进房间一次，如何确定每个开关对应哪盏灯？请给出推理过程。 |
| 5 | 课程相关 | 在 llama.cpp 中，GGUF 量化（如 Q4_K_M）相比 FP16 为什么能加速推理、又会带来什么代价？ |

**不同配置对质量的影响（如不同量化 / 不同 temperature）**：

| Prompt | 配置A（如 Q4_K_M, temp=0.7）评分 | 配置B（如 Q8_0）评分 | 备注 |
|--------|-------------|-------------|------|
| 1 | 【】 | 【】 | |
| 2 | 【】 | 【】 | |
| 3 | 【】 | 【】 | |
| 4 | 【】 | 【】 | |
| 5 | 【】 | 【】 | |

> 评分建议用 1–5 分人工打分（正确性 / 完整性 / 流畅性），并附上每个 prompt 的真实模型输出截图。
> 结论【待填写】：例如更高位量化在推理题(#4)上更稳定，低量化在简单问答上差异不明显。

### 2.6 基于 RPC 的多机分布式推理

**启用 RPC重新构建**：

```powershell
cd llama.cpp
cmake -B build-rpc -DGGML_RPC=ON
cmake --build build-rpc --config Release
```

**网络环境**：【待填写：两台机器 IP，如主机 192.168.x.A，从机 192.168.x.B；同一局域网 / 有线 or 无线】

**从机（worker）启动 rpc-server**：

```powershell
# 在从机上
.\build-rpc\bin\Release\rpc-server.exe -p 50052 -H 0.0.0.0
```

**主机（head）发起推理**：

```powershell
.\build-rpc\bin\Release\llama-cli.exe `
  -m .\models\qwen2.5-7b-instruct-q4_k_m.gguf `
  -p "请介绍分布式推理。" -n 128 `
  --rpc 192.168.x.B:50052
```

**部署命令与运行结果**：

```
【待填写：主机端成功推理的完整输出，能看到从机参与计算的日志（如 RPC backend 注册、层分配）】
```

### 2.7 单机 vs RPC 分布式推理性能对比

| 配置 | 加载时间 | Prefill (t/s) | Decode (t/s) | 备注 |
|------|----------|---------------|--------------|------|
| 单机（仅主机） | 【】 | 【】 | 【】 | |
| RPC（主机+1从机） | 【】 | 【】 | 【】 | |

**分析**【待填写】：RPC 带来的是收益还是开销？性能不要求提升，但需数据支撑并解释原因，例如：
- 网络延迟 / 带宽：RPC 每层都要在机器间传输激活值，无线网络下带宽成为瓶颈；
- 同步等待：主机需等待从机算完该层才能继续；
- 计算划分：层在两机间的划分是否均衡；
- 设备性能差异：从机更弱会拖慢整体（木桶效应）。

### 2.8（选做加分）

> 任选一项，每项 10 分。建议结合已有数据选 **不同量化格式对比 (Q4/Q5/Q8)**，成本最低。

**【待填写：选做方向 + 数据 + 分析】**

例：同模型 Q4_K_M / Q5_K_M / Q8_0 三种量化的体积、decode 吞吐、5 个 prompt 的质量评分对比表。

---

## 三、Ray：多机批量推理任务调度（选择性必做，20 分）

> 本组选择 **Ray** 方向（非 Ceph）。所有任务围绕 llama.cpp 主线展开。

### 3.1 Ray 环境部署（3 分）

**安装**：

```powershell
pip install "ray[default]"
```

**Head 节点**（主机）：

```powershell
ray start --head --port=6379 --dashboard-host=0.0.0.0
# 记录输出的 ray://<ip>:<port> 与集群地址
```

**Worker 节点**（从机，资源受限时可在同机用多进程模拟）：

```powershell
ray start --address='192.168.x.A:6379'
```

| 节点 | 角色 | IP | 说明 |
|------|------|----|----|
| 节点1 | head | 【】 | |
| 节点2 | worker | 【】 | 资源受限时说明单机多进程模拟方案 |

### 3.2 多节点 llama.cpp 推理服务（4 分）

在每个节点用 `llama-server` 起 HTTP 服务：

```powershell
.\llama.cpp\build\bin\Release\llama-server.exe `
  -m .\llama.cpp\models\qwen2.5-7b-instruct-q4_k_m.gguf `
  --host 0.0.0.0 --port 8080 -t 8
```

| 节点 | 模型 | 量化 | 启动命令 | 端口 |
|------|------|------|----------|------|
| 节点1 | 【】 | 【】 | 【】 | 8080 |
| 节点2 | 【】 | 【】 | 【】 | 8081 |

### 3.3 批量推理任务集（≥20 个 prompt，3 分）

设计 ≥20 个 prompt，覆盖课程知识问答 / 代码解释 / 摘要 / 自定义。建议存为 `Lab4/scripts/prompts.json`。

**【待填写：列出 20+ prompt，或附脚本文件链接】**

### 3.4 Ray 分发与指标采集（4 分）

用 Ray Task / Actor 把 prompt 分发到各 server，记录每个请求的开始时间、结束时间、总耗时、输出长度。建议脚本：`Lab4/scripts/ray_dispatch.py`。

```python
import ray, requests, time

ray.init(address="auto")
SERVERS = ["http://192.168.x.A:8080", "http://192.168.x.B:8081"]

@ray.remote
def infer(server, prompt):
    t0 = time.time()
    r = requests.post(f"{server}/completion",
                      json={"prompt": prompt, "n_predict": 128})
    t1 = time.time()
    out = r.json()["content"]
    return {"server": server, "start": t0, "end": t1,
            "dur": t1 - t0, "out_len": len(out)}

prompts = [...]  # 20+ prompts
# 轮询分配到不同 server
futures = [infer.remote(SERVERS[i % len(SERVERS)], p)
           for i, p in enumerate(prompts)]
results = ray.get(futures)
```

**采集结果**：【待填写：每请求的 start/end/dur/out_len 表格或汇总 CSV】

### 3.5 至少两种执行方式对比（4 分）

对比 **串行 / 单机并行 / 多机并行**（或固定分配 vs 轮询）：

| 执行方式 | 总耗时 (s) | 平均延迟 (s) | 吞吐 (req/s) |
|----------|-----------|-------------|-------------|
| 串行 | 【】 | 【】 | 【】 |
| 单机并行 | 【】 | 【】 | 【】 |
| 多机并行 | 【】 | 【】 | 【】 |

### 3.6 实验现象分析（2 分）

**【待填写】**：分析 Ray 调度开销、模型加载复用（server 常驻避免重复加载）、节点性能差异、网络开销、请求粒度（短 prompt 时调度开销占比更高）对结果的影响。

### 3.7（Ray 选做加分，最高 10 分）

任选：负载均衡调度 / 失败重试 / 异构节点分析 / 并发压力测试。

**【待填写：选做方向 + 实现说明 + 运行命令 + 数据 + 分析】**

---

## 四、附录

- 实验脚本：`Lab4/scripts/`（【待补充】）
- 命令记录与配置：见各节命令块
- 结果截图：`Lab4/docs/img/`（【待补充】）
