# Mini9P Client 接口详解

本文档讲解 `pwos-master-esp32p4/mini9p/mini9p_client.h` 中的函数与数据结构，对应 9P 协议的客户端实现。

---

## 数据结构先览

### `m9p_transport_fn` 传输函数指针
（第 14-20 行）— 传输函数指针类型，用于实际收发数据：
- `transport_ctx`：传输上下文（如 UART/SPI 句柄）
- `tx_data` / `rx_data`：发送/接收缓冲区
- `tx_len` / `rx_cap`：发送长度 / 接收容量
- `rx_len`：实际接收长度（输出）
- 返回值：发送的字节数或错误码

---

### `struct m9p_client` 客户端状态结构体
（第 22-35 行）— 保存客户端完整状态：
| 字段 | 说明 |
|------|------|
| `transport` / `transport_ctx` | 底层传输函数与上下文 |
| `next_tag` / `next_fid` | 自增的标签和文件句柄（FID） |
| `negotiated_msize` | 与服务器协商的帧大小 |
| `max_fids` / `max_inflight` | 最大文件数和最大并发请求数 |
| `feature_bits` | 特性位掩码 |
| `attached` | 是否已成功 attach |
| `root_qid` | 根节点的 QID |
| `tx_buffer` / `rx_buffer` | 收发缓冲区（默认 512 字节，`M9P_CLIENT_BUFFER_CAP`） |

---

## 函数逐个讲解

### `m9p_client_init`
（第 37 行）初始化客户端结构体，绑定传输函数和上下文。调用后客户端处于未连接状态，需先调用 `m9p_client_attach` 建立会话。

---

### `m9p_client_alloc_fid`
（第 38 行）分配一个新的 FID（文件标识符），从 `M9P_FIRST_DYNAMIC_FID`（值为 1）开始自增。返回分配的 FID，使用完毕后需调用 `m9p_client_clunk` 释放。

---

### `m9p_client_attach`
（第 40-44 行）向服务器发起 `Tattach` 请求，建立会话。
- `requested_msize`：客户端期望的最大帧大小
- `requested_inflight`：最大并发请求数
- `attach_flags`：附加标志位
- 成功后 `attached` 置为 `true`，`negotiated_msize` 和 `root_qid` 被服务器返回值填充

---

### `m9p_client_walk`
（第 45 行）向服务器发起 `Twalk` 请求，从 `fid` 对应的文件开始，沿 `path` 路径遍历到 `newfid`。
- `fid`：起始目录的 FID
- `newfid`：目标文件的 FID（通常为新分配）
- `path`：要遍历的路径（如 `"led/status"`）
- `out_qid`：输出目标文件的 QID
- 用途：进入子目录或定位文件

---

### `m9p_client_open`
（第 46 行）向服务器发起 `Topen` 请求，打开一个文件。
- `fid`：目标文件的 FID（通常由 walk 得到）
- `mode`：打开模式（读/写/读写）
- `out_result`：输出打开结果（含 I/O 单元大小 `iounit`）

---

### `m9p_client_read`
（第 47-52 行）向服务器发起 `Tread` 请求，从文件中读取数据。
- `fid`：已打开文件的 FID
- `offset`：读取偏移量
- `data`：接收缓冲区
- `in_out_count`：**传入**期望读取的字节数，**返回**实际读取的字节数

---

### `m9p_client_write`
（第 53-59 行）向服务器发起 `Twrite` 请求，向文件写入数据。
- `fid`：已打开文件的 FID
- `offset`：写入偏移量
- `data`：待写入数据
- `count`：期望写入的字节数
- `out_written`：输出实际写入的字节数

---

### `m9p_client_stat`
（第 60 行）向服务器发起 `Tstat` 请求，获取文件元数据（大小、类型、权限等）。
- `fid`：目标文件的 FID
- `out_stat`：输出文件状态信息（对应 `struct m9p_stat`）

---

### `m9p_client_clunk`
（第 61 行）向服务器发起 `Tclunk` 请求，关闭文件并释放 FID。调用后该 FID 不再有效。

---

### `m9p_client_walk_path`
（第 63 行）便捷函数，封装了 `walk` 的常见用法：传入路径，自动分配 `newfid` 并完成遍历。
- `path`：要访问的路径
- `out_fid`：输出得到的 FID
- `out_qid`：输出目标 QID
- 等价于 `alloc_fid` + `walk` 的组合

---

### `m9p_client_open_path`
（第 64-69 行）便捷函数，封装了 `walk` + `open` 的两步操作：直接传入路径，一步完成打开。
- `path`：要打开的路径
- `mode`：打开模式
- `out_fid`：输出打开后的 FID
- `out_result`：输出打开结果
- 等价于 `walk_path` + `open` 的组合，是最常用的高层接口

---

## 总结

| 层级 | 函数 | 说明 |
|------|------|------|
| 低层 | `walk` / `open` / `read` / `write` / `clunk` | 对应 9P 协议原语，灵活但需手动管理 FID 和 walk 流程 |
| 高层 | `walk_path` / `open_path` | 封装常见组合，直接按路径操作文件，减少重复代码 |

**典型使用流程**：
`init` → `attach` → `open_path`（或 `walk` + `open`）→ `read` / `write` → `clunk`
