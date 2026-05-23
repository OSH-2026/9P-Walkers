# 计算任务设计：Mandelbrot / Julia 分形渲染（推荐先行任务）

>  Author：wwt
>  调研协作：Claude (claude-opus-4-7)
>  日期：2026-05-23

---

## 0. 为什么先做分形，再做路径追踪

这份文档是 [`pathtracing_render.md`](./pathtracing_render.md) 的**配套先行方案**。
路径追踪是很好的压轴 showcase，但工程链路长、风险点多。**Mandelbrot 分形能用几乎相同的
调度 + 显示框架，以低得多的风险先把整条「计算任务下发 → 集群计算 → WebShell 出图」打通。**

| 维度 | 路径追踪 | Mandelbrot 分形 |
|------|----------|-----------------|
| 任务参数 | 场景 + tile + 种子 | **仅复平面区域 + 迭代上限**（≤32B） |
| 是否需要场景数据 | 需要（硬编码场景） | **完全不需要** |
| slave 计算复杂度 | 高（多次球求交 + 弹射） | **低（每像素一个 z²+c 迭代循环）** |
| 是否递归 | 是（需改迭代） | **本就是迭代，零改写** |
| 浮点需求 | 单精度勉强 | **单精度足够**（深缩放才需更高精度） |
| 数值健壮性 | 易有噪点/偏色 | **结果确定、无噪声** |
| 视觉效果 | 强 | **强（且可做缩放动画）** |
| STM32F411 适配度 | 紧张 | **宽裕** |

> 结论：**强烈建议把 Mandelbrot 作为 P0/P1 的第一个真实计算任务**。它跑通后，路径追踪只是
> 把 slave 上的「分形 kernel」换成「路径追踪 kernel」，调度层与 WebShell 显示层完全复用。

---

## 1. 算法极简回顾

对图像上每个像素 (px, py)，映射到复平面点 c = (re, im)，迭代：

```
z = 0
for n in 0..max_iter:
    z = z*z + c          # 复数运算：(zr,zi) -> (zr²-zi²+cr, 2·zr·zi+ci)
    if |z|² > 4: break   # 逃逸
color = palette(n)        # 用逃逸迭代次数 n 映射到颜色
```

- 每像素**完全独立** → 与路径追踪同样「embarrassingly parallel」
- 无随机数、无场景、无递归、无 malloc —— 完美符合 slave 设计规则（architecture.md §6）
- Julia 集只是把 c 固定、z 初值设为像素坐标，**同一套 kernel 加个开关即可**

---

## 2. 硬件适配性

| 项目 | 评估 |
|------|------|
| STM32F411 算力 | 每像素 max_iter 次复数乘加（约 6 flops/iter）。256×256 @ max_iter=256 ≈ 256×256×256×6 ≈ **1 亿 flops**，M4F@100MHz 单精度约 **1–3 秒/帧**（单节点、未优化） |
| 内存 | 一个 tile 的 RGB 缓冲（如 32×32×3=3KB）即可，128KB SRAM 绰绰有余 |
| 浮点 | 单精度 float 在常规视野下足够；**深缩放**（zoom 超过 ~1e5）才会出现像素化，那时需 double 或定点扩展 |
| 传输 | 与路径追踪完全相同的瓶颈（mini9P 256B/次 iounit，UART ~8KB/s）→ 见 pathtracing_render.md §2.2 |

> 分形的计算/传输比比路径追踪更「划算」：计算可调（max_iter），传输量固定。
> 可通过提高 max_iter 让 demo「看起来在认真算」，同时画面更精细。

---

## 3. 任务分解与 9P 映射（与路径追踪共用框架）

### 3.1 slave 虚拟文件树
```
/mcuN/
└── compute/
    └── fractal/
        ├── job        (write)  ← master 写入 tile 区域参数
        ├── result     (read)   ← master 读回 tile RGB
        └── status     (read)   ← 可选状态
```

### 3.2 任务参数包（≤32B，远小于 512B msize）
```c
struct fractal_job {
    float    re_min, re_max;   // 该 tile 对应复平面横向范围
    float    im_min, im_max;   // 纵向范围
    uint16_t tile_w, tile_h;   // tile 像素尺寸
    uint16_t max_iter;         // 迭代上限（决定精度与耗时）
    uint8_t  mode;             // 0=Mandelbrot, 1=Julia
    uint8_t  palette_id;       // 调色板选择
    float    julia_cr, julia_ci; // Julia 模式下的常数 c（Mandelbrot 忽略）
};  // ≈ 30 字节
```

### 3.3 master 侧 Lua 调度（与路径追踪同构）
```lua
local TILE = 32
local W, H = 256, 256
-- 整幅图对应的复平面视野
local view = {re0=-2.5, re1=1.0, im0=-1.25, im1=1.25}
for ty = 0, H-1, TILE do
  for tx = 0, W-1, TILE do
    local node = pick_idle_node()
    vfs.write(node.."/compute/fractal/job", pack_fractal_job(tx, ty, TILE, view, 256))
    local pixels = vfs.read(node.."/compute/fractal/result")
    push_tile_to_webshell(tx, ty, TILE, TILE, pixels)
  end
end
```

---

## 4. WebShell 显示

**与路径追踪完全相同**，直接复用 `pathtracing_render.md §4` 的三套方案：
- 方案 A：ANSI 真彩色块（零前端改动，验证链路）
- 方案 B：**Canvas + `TILE x y w h base64` 渐进式推送（推荐主展示）**
- 方案 C：整帧 BMP/PNG 导出

> 分形特别适合**缩放动画**：master 每帧缩小复平面视野（re/im 范围按比例收缩），
> 连续下发多帧，WebShell 上就是一段「无限放大」的分形 zoom 演示 —— 视觉冲击力极强，
> 且每帧都在真实地分布式计算。

---

## 5. 可参考的开源资料

| 资源 | 用途 |
|------|------|
| Wikipedia "Mandelbrot set" — Escape time algorithm 伪代码 | 算法权威参考，直接照抄 |
| "Smooth iteration count / normalized iteration count" | 去色带（banding），让边缘平滑：`n + 1 - log(log|z|)/log 2` |
| 各类 "mandelbrot C" / "fixed point mandelbrot" 实现 | 单精度/定点参考，嵌入式友好 |
| 调色板：HSV→RGB 或预置 256 色 LUT | tile kernel 末端把 iter 数映射成颜色 |

> 分形实现网上极其丰富且短小（核心循环 ~15 行），**几乎没有移植风险**，比路径追踪的
> kernel 移植容易一个数量级。

---

## 6. 分阶段实施计划

| 阶段 | 目标 | 验收 |
|------|------|------|
| **F0** | slave 实现 `fractal/{job,result}` 节点 + 单精度 Mandelbrot kernel（单 tile） | 单 tile 像素正确，实测耗时 |
| **F1** | master 调度全部 tile + WebShell 方案 A（ANSI）出图 | 终端能看到分形轮廓 |
| **F2** | WebShell 方案 B（Canvas 渐进式），整帧 256×256 | 浏览器完整画出 Mandelbrot |
| **F3** | 缩放动画（master 连续下发收缩视野的多帧） | WebShell 上看到 zoom 动画 |
| **F4（增强）** | 多 slave 并行；Julia 模式切换 | 渲染时间随节点数下降；可切换 Julia |

完成 F0–F2 后，**路径追踪只需替换 slave kernel 与参数包**，直接进入 pathtracing 的 P2 阶段。

---

## 7. 两个任务的关系总结

```
        ┌─────────────────────────────────────────────┐
        │  共用框架（先用 Mandelbrot 打通，零风险）       │
        │  - master tile 调度（Lua）                    │
        │  - cluster_vfs 任务下发 / 结果回读             │
        │  - WebShell Canvas 渐进式显示（TILE 子协议）   │
        └─────────────────────────────────────────────┘
                    ↑                      ↑
          替换 slave kernel        替换 slave kernel
                    │                      │
          ┌──────────────────┐   ┌──────────────────────┐
          │ Mandelbrot 分形   │   │ PathTracing 路径追踪   │
          │ （先行 / 低风险）  │   │ （压轴 / 高视觉冲击）   │
          └──────────────────┘   └──────────────────────┘
```

**建议路线：先 Mandelbrot 跑通全链路（F0–F3），再把路径追踪作为压轴 showcase 接入。**

---

*本文为可行性调研，与 `pathtracing_render.md` 共用调度与显示框架。*
