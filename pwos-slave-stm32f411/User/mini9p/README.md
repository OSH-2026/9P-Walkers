# Mini9P Server 与 Service

`User/mini9p` 放置 STM32F411 节点侧 Mini9P 协议、server 状态机和 mesh 组装层。

## 模块边界

```text
mini9p_protocol  帧编解码、T* 解析、R* 构造
mini9p_server    会话状态、fid 生命周期、请求分发、Rerror
mesh_uart_transport  基于 USART 收发完整 mesh frame
mini9p_service       上板组装层，串起 lfs_vfs + server + mesh_node_runtime + UART
```

`mini9p_server` 不直接访问 littlefs 或设备资源。它只通过 `m9p_server_ops` 调用 backend。

## Mini9P Server

核心文件：

- `mini9p_server.h`
- `mini9p_server.c`

server 负责：

- 处理 `Tattach/Twalk/Topen/Tread/Twrite/Tstat/Tclunk`
- 管理 attach 会话状态
- 管理 fid 表：`fid -> path/qid/open/mode/iounit`
- 校验 fid、路径、打开状态和读写权限
- 调用 backend 的 `stat/open/read/write/clunk`
- 将错误统一编码为 `Rerror`

唯一入口：

```c
int m9p_server_handle_frame(void *server_ctx,
                            const uint8_t *request_data,
                            size_t request_len,
                            uint8_t *response_data,
                            size_t response_cap,
                            size_t *response_len);
```

典型初始化：

```c
struct m9p_server server;
struct m9p_server_config config;

m9p_server_get_default_config(&config);
config.ops = lfs_vfs_ops();
config.ops_ctx = &lfs_vfs;
m9p_server_init(&server, &config);
```

## Mini9P Service

核心文件：

- `mini9p_service.h`
- `mini9p_service.c`

service 是 STM32 上板联调用的薄组装层，内部持有静态实例：

- `lfs_vfs`
- `m9p_server`
- `mesh_uart_transport`
- `mesh_node_runtime`

默认还会在 littlefs 中写入一组调试文件，便于通过 Mini9P 远程检查 `/verify` 树，而不往 mesh UART 混入文本日志。

对外只暴露两个入口：

```c
int mini9p_service_init(void);
int mini9p_service_notify_link_up(void);
int mini9p_service_poll_once(void);
```

初始化顺序：

```text
lfs_vfs_init
  -> fs_selftest_run_on_fs
  -> m9p_server_init(lfs_vfs_ops)
  -> mesh_uart_transport_init
  -> mesh_node_runtime_init(send_frame/receive_frame + m9p_server_handle_frame)
  -> auto REGISTER
```

轮询流程：

```text
mini9p_service_poll_once
  -> mesh_node_runtime_poll_once
  -> mesh_processer_process_frame
  -> local T* -> m9p_server_handle_frame -> lfs_vfs
```

在当前 F411 组装层里，`Core/Src/main.c` 通过该 service 轮询 mesh/Mini9P。此模式下承载 mesh 的 UART（当前实现为 USART1）应由二进制帧独占，不混入 VOFA 或其他文本日志。

## PC 测试

server 单元测试：

```bash
cmake -S pwos-slave/User/mini9p/test -B pwos-slave/User/mini9p/test/build
cmake --build pwos-slave/User/mini9p/test/build
pwos-slave/User/mini9p/test/build/mini9p_server_test
```

上板串口联调工具见：

```text
tools/pc_master_emulator/
```

当前 smoke test 目标是通过 Mini9P 读取 `/verify/fs_selftest.txt`、`/verify/debug/report.txt` 和 `/verify/tree/nested/config.txt`，确认整条 UART -> mesh -> mini9p -> lfs_vfs -> littlefs 链已经打通。
