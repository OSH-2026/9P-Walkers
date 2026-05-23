# 计算任务设计：PathTracing 路径追踪渲染器

>  Author：wwt
>  调研协作：Claude (claude-opus-4-7)
>  日期：2026-05-23

---

## 0. 一句话结论

**可行，但定位为「分布式调度能力的可视化演示」，而非实时渲染。**
在低分辨率（≤256×256）、低采样（4–16 spp）、简单场景（Cornell Box / 几个球体）下，
路径追踪能很好地展示 9P 集群「拆分任务 → 下发 → 收集 → 合并」的完整链路，并在 WebShell 上
产出有冲击力的画面。但受限于 STM32F411 算力与 **UART/mini9P 的传输带宽**，整帧渲染时间会在
**数十秒量级**，不要期待交互级帧率。

> 建议：把路径追踪当作「压轴 showcase」，而先用更轻量的 **Mandelbrot 分形**（见同目录
> [`mandelbrot_render.md`](./mandelbrot_render.md)）打通整条「计算任务下发 + WebShell 出图」
> 的工程链路。两者复用同一套调度与显示框架，分形跑通后路径追踪只是替换 kernel。

---

## 1. 为什么路径追踪适合 9P 集群

| 特性 | 说明 | 对 9P 的意义 |
|------|------|--------------|
| **天然并行** | 每个像素 / tile 的采样彼此独立，无数据依赖 | 完美契合「master 拆 tile → 多 slave 并行 → 合并」模型 |
| **任务自包含** | 一个 tile 的渲染只需要「场景 + tile 坐标 + 随机种子」 | 任务参数极小（≤512B），契合 mini9P 的 msize 限制 |
| **结果可增量** | 每个 tile / 每轮采样都能独立产出、累加 | 支持渐进式出图（progressive rendering），WebShell 可边算边显示 |
| **与现有设计对齐** | `architecture.md` 已规划 `/mcuN/compute/...` 的「写参数→读结果」范式 | 不需要新增协议，复用 cluster_vfs |

这套「embarrassingly parallel」的结构，正是分布式计算最容易出彩、也最容易讲清楚的 demo。

---

## 2. 硬件现实检查（Reality Check）

### 2.1 算力

| 平台 | 核心 | 浮点 | 内存 | 评估 |
|------|------|------|------|------|
| **ESP32-P4**（master） | 双核 RISC-V HP @ ~400MHz | 单精度 FPU | 片内 ~768KB + 可选 PSRAM | 适合做调度 + framebuffer 汇总 + 编码。也可自己分担 tile |
| **STM32F411**（slave） | Cortex-M4F @ 100MHz | **仅单精度** FPU，无双精度硬件 | 128KB SRAM / 512KB Flash | 计算节点，单精度够用但要避免 double |

**关键限制：**
- smallpt 等经典实现用 `double`，移植到 STM32F411 **必须改为 `float`**，否则会走软件浮点、慢一个数量级。低分辨率下单精度的误差可接受。
- 设计规则要求 slave **禁止 malloc / 禁止深递归**（见 architecture.md §6）。路径追踪天然递归，需**改写为迭代式光线弹射循环**，固定最大深度，使用固定大小的栈数组。

### 2.2 传输带宽（这是真正的瓶颈）

实测/理论参数：
- UART **115200 8N1** → 原始 ~11.5 KB/s；扣除 mini9P 帧头 + CRC + 半双工换向开销，**有效负载约 6–9 KB/s**
- mini9P 单次 IO：**msize = 512B，iounit = 256B**（已确认：`mini9p_client.h` / `mini9p_server.c`）
  → 每个 Rread 最多搬运 **256 字节** 像素数据

**整帧传输时间估算（单链路）：**

| 分辨率 | RGB888 字节数 | 256B/次 → 读取次数 | @8KB/s 纯传输 |
|--------|--------------|-------------------|---------------|
| 128×128 | 49,152 | ~192 | ~6 s |
| 256×256 | 196,608 | ~768 | ~24 s |
| 512×512 | 786,432 | ~3072 | ~96 s |

> 结论：**传输和计算同量级，甚至传输更贵**。必须用 tile 流水线（一个 tile 在传输时，下一个 tile 在计算）来掩盖延迟；多 slave 时传输瓶颈在 master 的 UART 总线上会进一步凸显（半双工共享）。

### 2.3 计算时间（数量级估算，需上板实测校准）

以 smallpt 风格、场景为「Cornell Box + 数个球」、float、最大弹射深度 4 为例：
- 单条采样路径（含若干次球求交）在 M4F@100MHz 上约 **10–50 µs**（粗估）
- 一个 32×32 tile @ 8 spp = 1024 px × 8 = 8192 条路径 → **约 0.1–0.4 s / tile**

**256×256 @ 8spp（64 个 32×32 tile）端到端估算：**
- 单 slave：64 × (≈0.25s 计算 + ≈0.4s 传输) ≈ **40 s**
- 双 slave 流水线：**约 20–25 s**

> 以上为 order-of-magnitude 估计，**务必在真机上用 `esp_timer` / DWT cycle counter 实测**后再写进报告。

---

## 3. 任务分解与 9P 映射

### 3.1 文件树设计（slave 侧虚拟节点）

```
/mcuN/
└── compute/
    └── render/
        ├── job        (write)  ← master 写入 tile 任务参数
        ├── result     (read)   ← master 读回该 tile 的 RGB 像素
        └── status     (read)   ← 可选：查询 "idle"/"busy"/"done"
```

### 3.2 任务参数包（写入 `/mcuN/compute/render/job`，≤512B）

场景本身**硬编码在 slave Flash 中**（Cornell Box / 固定球体列表），只下发可变参数：

```c
struct render_job {
    uint16_t tile_x, tile_y;     // tile 左上角像素坐标
    uint16_t tile_w, tile_h;     // tile 尺寸（如 32×32）
    uint16_t img_w,  img_h;      // 整帧尺寸（slave 据此算 ray 方向）
    uint16_t samples;            // 每像素采样数 spp
    uint8_t  max_depth;          // 最大弹射深度
    uint8_t  scene_id;           // 选择哪个内置场景
    uint32_t seed;               // 该 tile 的 RNG 种子（保证可复现）
};  // ≈ 22 字节，远小于 512B
```

### 3.3 结果回读（`/mcuN/compute/render/result`）

- slave 把渲染好的 tile 存进静态 framebuffer（`uint8_t tile_rgb[32*32*3]` = 3072B，128KB SRAM 完全放得下）
- master 用 `cluster_vfs_read_path` 分块（256B/次）读回，按 tile 坐标贴进整帧 framebuffer
- 像素格式建议 **RGB888**；若要省带宽可用 **RGB565**（省 1/3），代价是色深

### 3.4 调度逻辑（master 侧，Lua 编排）

```lua
-- 伪代码：tile 工作队列分发
local TILE = 32
local W, H = 256, 256
for ty = 0, H-1, TILE do
  for tx = 0, W-1, TILE do
    local node = pick_idle_node()                 -- 轮询/负载均衡选节点
    vfs.write(node.."/compute/render/job", pack_job(tx,ty,TILE,TILE,W,H,8,4,seed()))
    local pixels = vfs.read(node.."/compute/render/result")
    blit_to_framebuffer(tx, ty, pixels)           -- 贴回整帧
    push_tile_to_webshell(tx, ty, TILE, TILE, pixels)  -- 渐进式推送到浏览器
  end
end
```

> 注：当前 `cluster_vfs` / Lua 绑定（`vfs.read/write`）已就绪（见 PR `feat/lua-vfs-bindings`）。
> 真正的多节点并行还需 Cluster Manager 支持节点注册与负载均衡（尚未实现，见 §6）。

---

## 4. 如何在 WebShell 上呈现渲染结果

现有前端是 `index.html` + xterm.js，通过 WebSocket 与 master 双向通信。三种由易到难的方案：

### 方案 A：ANSI 真彩色块（零前端改动，立即可用）
- 用终端的 24-bit color escape（`\x1b[48;2;R;G;Bm`）+ 半块字符 `▀`，一个字符格显示上下两个像素
- 终端 ~80 列 → 最多约 80×48 像素，**只能看个轮廓**，但**不需要改前端**，适合最早期验证链路
- master 把 framebuffer 转成 ANSI 字符串，直接走现有 `websocket_shell_broadcast`

### 方案 B：Canvas + 渐进式 tile 推送（**推荐**）
- 前端新增一个「渲染」标签页，放一个 `<canvas>`
- 定义一个 WebSocket 子协议（文本帧即可），例如：
  ```
  TILE <x> <y> <w> <h> <base64-of-RGB888>
  ```
- 前端 JS 收到后 base64 解码 → 写入 `ImageData` → `ctx.putImageData(imgData, x, y)`
- **优点**：master 侧几乎不用编码（只做 base64），画面**边算边出**，视觉效果最佳，传输增量最小
- base64 膨胀 ~33%，可接受；若用 WebSocket 二进制帧则无膨胀

```javascript
// 前端示意
ws.onmessage = (ev) => {
  if (ev.data.startsWith('TILE ')) {
    const [_, x, y, w, h, b64] = ev.data.split(' ');
    const raw = atob(b64);
    const img = ctx.createImageData(+w, +h);
    for (let i = 0, p = 0; i < raw.length; i += 3, p += 4) {
      img.data[p]   = raw.charCodeAt(i);
      img.data[p+1] = raw.charCodeAt(i+1);
      img.data[p+2] = raw.charCodeAt(i+2);
      img.data[p+3] = 255;
    }
    ctx.putImageData(img, +x, +y);
  } else {
    term.write(ev.data);   // 普通 shell 输出仍走 xterm
  }
};
```

### 方案 C：整帧编码为 PNG/BMP（一次性出图）
- master 把整帧编码成图片，前端用 `<img src="data:image/png;base64,...">` 显示
- PNG 需要 zlib（可用 `miniz`/`uzlib`，但占 Flash/RAM）；**BMP 几乎零成本**（无压缩，加个 54 字节头即可）
- 缺点：必须等整帧算完才出图，无渐进感；且未压缩 BMP 体积大、传输久
- 适合「最终成片下载/导出」，不适合实时展示

> **推荐路线**：方案 A 验证链路 → 方案 B 做主展示（渐进式 Canvas）→ 方案 C 作为「导出成片」附加功能。

---

## 5. 可参考的开源仓库 / 技术资料

### 5.1 极简路径追踪器（移植 kernel 的首选）
| 项目 | 语言/规模 | 用途 | 备注 |
|------|----------|------|------|
| **smallpt**（Kevin Beason） | C++，99 行 | 路径追踪「黄金参考」，含 Cornell Box | 用 double，需改 float；OpenMP 部分删掉 |
| **Ray Tracing in One Weekend**（Peter Shirley） | C++，配套书 | 最佳教学路径，逐步构建 | 概念清晰，易抽出 float 版核心循环 |
| **ssloy/tinyraytracer** | C++，256 行 | 可读性极高的 step-by-step | GitHub 上有完整 wiki 讲解 |
| **ssloy/tinykaboom** | C++ | ray marching 变体 | 备选 kernel |

### 5.2 嵌入式 / MCU 上的光线追踪先例（证明可行性）
- 搜索关键词：`raytracer STM32`、`raytracer Arduino`、`Cortex-M raytracer` —— 已有多个 hobby 项目在 AVR/STM32 上跑出 Cornell Box（分辨率极低、耗时长，但**证明 MCU 能跑**）
- `fenbf` / 各类「raytracer on microcontroller」博客与 Hackaday 项目

### 5.3 辅助组件
| 需求 | 推荐 |
|------|------|
| 随机数（每 tile 可复现） | **xorshift32** / **PCG-XSH-RR**（几行代码，无状态依赖） |
| PNG 编码（方案 C） | `lodepng`（单文件）或 `uzlib` + 手写 PNG chunk |
| BMP 编码（方案 C 轻量版） | 手写 54 字节 BITMAPFILEHEADER+INFOHEADER，**最简单** |
| 数据压缩（可选省带宽） | `miniz` / `uzlib`（注意 RAM 占用） |

---

## 6. 前置依赖与风险

### 6.1 依赖（按必要性排序）
1. ✅ **cluster_vfs + Lua `vfs.*` 绑定**：已就绪（`feat/lua-vfs-bindings`），任务下发/回读链路可用
2. ✅ **WebSocket 广播**：已就绪（`feat/web-shell-broadcast`），文本帧推送可用
3. ⚠️ **slave 计算节点框架**：需在 STM32F411 上实现 `/compute/render/{job,result}` 虚拟节点 + float 路径追踪 kernel（迭代式、无 malloc）
4. ⚠️ **WebShell Canvas 标签页**：需在 `index.html` 增加 canvas + `TILE` 子协议解析（方案 B）
5. ❌ **多节点并行调度**：需 Cluster Manager 支持节点注册与负载均衡（未实现）。**单 slave 即可先做 demo**，多节点是增强项

### 6.2 主要风险
| 风险 | 影响 | 缓解 |
|------|------|------|
| 传输带宽瓶颈 | 整帧数十秒 | tile 流水线、RGB565、降分辨率、后续升级 SPI/WiFi 传输 |
| slave 算力弱 + 单精度 | 慢、有噪点 | 降 spp / 降分辨率 / 简单场景；把误差当「艺术风格」讲 |
| 递归改迭代易出 bug | 渲染错误 | 先在 PC 上用相同 float kernel 验证，再移植 |
| cluster_vfs 无并发锁 | UART/WS 同时调用竞争 | demo 期间串行调度；后续加互斥锁 |
| 整帧 framebuffer 内存 | 256×256×3=192KB | 放 ESP32-P4（可借 PSRAM）；slave 只存单 tile |

---

## 7. 分阶段实施计划

| 阶段 | 目标 | 验收标准 |
|------|------|----------|
| **P0 链路验证** | slave 实现一个「填充纯色 tile」的假 kernel；master 下发→回读→ANSI 显示（方案 A） | WebShell 终端能看到色块拼出的图案 |
| **P1 PC 端 kernel** | 在 PC 上用 float 写好迭代式路径追踪，渲染 Cornell Box 正确 | 输出图与参考一致 |
| **P2 slave 移植** | 把 P1 的 kernel 移植到 STM32F411，单 tile 渲染 + 回读 | 单 tile 像素正确，实测单 tile 耗时 |
| **P3 整帧 + Canvas** | master 调度全部 tile，前端方案 B 渐进式出图 | 浏览器 Canvas 上完整渲染出 256×256 画面 |
| **P4 多节点（增强）** | 接入 Cluster Manager，2+ slave 并行 | 渲染时间随节点数近线性下降 |
| **P5 优化（可选）** | RGB565 / SPI 传输 / 渐进采样累加 | 整帧时间显著下降 |

---

## 8. 给报告/答辩的叙事建议

这个 demo 的**卖点不是渲染速度，而是「操作系统把异构算力组织起来做一件事」**：
- 「一切皆文件」：渲染任务就是往 `/mcuN/compute/render/job` 写几个字节
- 「控制平面在 master」：Lua 脚本几行就完成 tile 拆分与调度
- 「计算平面在 slave」：STM32 只管闷头算自己那块
- 「可视化」：WebShell 上画面一块块浮现，直观展示并行与调度

把「慢」转化为叙事优势：**让观众肉眼看到每个 tile 由哪个节点算出来**（比如不同节点用不同色调的进度边框），并行调度的过程本身就是最好的演示。

---

*本文为可行性调研，kernel 与协议细节将在 P1/P2 阶段补充到 `protocol_spec.md`。*
