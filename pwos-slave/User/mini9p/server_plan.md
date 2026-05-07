# Mini9P Server 实施计划

## 目标

完成 STM32 侧 mini9P server，跑通第一条主从文件访问闭环：

```text
ESP32 cluster_vfs
  -> mini9p_client
  -> UART request
  -> STM32 mini9p_server
  -> 本地文件/虚拟节点
  -> STM32 mini9p_server
  -> UART response
  -> mini9p_client
  -> ESP32 cluster_vfs
```

## 1. 先补齐协议层

当前协议辅助函数主要偏 client 侧：

```text
构造 T* 请求
解析 R* 响应
```

server 侧需要补齐相反方向：

```text
解析 T* 请求
构造 R* 响应
```

需要新增请求解析：

```text
m9p_parse_tattach
m9p_parse_twalk
m9p_parse_topen
m9p_parse_tread
m9p_parse_twrite
m9p_parse_tstat
m9p_parse_tclunk
```

需要新增响应构造：

```text
m9p_build_rattach
m9p_build_rwalk
m9p_build_ropen
m9p_build_rread
m9p_build_rwrite
m9p_build_rstat
m9p_build_rclunk
m9p_build_rerror
```

协议定义保持 client/server 共用，不拆成两套不兼容的协议文件。

## 2. 实现 server 核心

在 `mini9p_server.h/.c` 中增加轻量 server 状态：

```text
attached 标志
fid 表
fid -> path/qid/open 状态
协商后的 msize
根目录 qid
```

对外提供一个能直接接入 `m9p_uart_transport_serve_once()` 的处理函数：

```text
m9p_server_handle_frame(...)
```

handler 主要流程：

```text
解码 frame
按 T* type 分发
校验 fid/path/mode
调用本地 backend
构造 R* 或 Rerror
```

实现时需要对照 master 侧 `mini9p_client.c`：

```text
m9p_client_attach    <-> handle_tattach
m9p_client_walk      <-> handle_twalk
m9p_client_open      <-> handle_topen
m9p_client_read      <-> handle_tread
m9p_client_write     <-> handle_twrite
m9p_client_stat      <-> handle_tstat
m9p_client_clunk     <-> handle_tclunk
```

重点确认：

```text
payload 字段顺序一致
响应 type 正确
tag 原样返回
fid 生命周期一致
错误统一返回 Rerror
```

## 3. 建立模块 testbench

在 PC 上先跑 server 模块测试，避免所有问题都等上板后才暴露。

建议目录：

```text
pwos-slave/User/mini9p/test/
```

第一版 testbench 只验证协议和 server 状态机：

```text
client 构造 T* 请求
server handle frame
server 返回 R* 响应
client parse R* 成功
```

优先覆盖：

```text
Tattach -> Rattach
Twalk /sys/health -> Rwalk
Topen -> Ropen
Tread -> Rread
Tclunk -> Rclunk
非法路径 -> Rerror ENOENT
非法 fid -> Rerror EFID
```

testbench 使用 fake backend，不依赖 UART、HAL 或 littlefs。

## 4. 先做最小 backend

先做只读虚拟节点，不急着完整接 littlefs：

```text
/sys/health
/verify/fs_selftest.txt
/
```

等请求/响应闭环稳定后，再通过 `lfs_port_fs()` 接入 littlefs。

## 5. MVP 请求集合

第一阶段必须跑通：

```text
Tattach -> Rattach
Twalk   -> Rwalk
Topen   -> Ropen
Tread   -> Rread
Tclunk  -> Rclunk
Rerror  -> 所有非法情况
```

第二阶段再补：

```text
Tstat   -> Rstat
Twrite  -> Rwrite
目录读取
```

## 6. 工程集成

把这些源文件加入 `pwos-slave/CMakeLists.txt`：

```text
User/mini9p/mini9p_protocol..c
User/mini9p/mini9p_server.c
```

第一版稳定后，建议把 `mini9p_protocol..c` 改名为 `mini9p_protocol.c`。

## 7. 验证标准

最小成功标准：

```text
PC testbench 可以跑通只读闭环
master 可以 attach 到 STM32
master 可以 open/read/clunk 一个文件
非法路径返回 Rerror，而不是卡死
一次 request 只产生一次 response
cluster_vfs_read_path("/mcu1/...", ...) 能读到数据
```
