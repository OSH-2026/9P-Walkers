# Mini9P Server 与 Service

`User/mini9p` 放置 STM32 从机侧 Mini9P 协议、server 状态机和 UART 组装层。

## 模块边界

```text
mini9p_protocol  帧编解码、T* 解析、R* 构造
mini9p_peer_link 同链路双向请求分发：区分本端等待的 R* 与对端主动发来的 T*
mini9p_server    会话状态、fid 生命周期、请求分发、Rerror
uart_transport   基于 USART 收发完整 Mini9P frame
mini9p_service   上板组装层，串起 local_vfs + server + peer_link + UART
```

`mini9p_server` 不直接访问 littlefs 或设备资源。它只通过 `m9p_server_ops` 调用 backend。

`mini9p_peer_link` 位于 raw transport 和 `mini9p_server` 之间，用来解决这样的问题：

- 本端正在等待某个请求的 R* 响应。
- 但链路上先到的是对端主动发来的 T* 请求。

没有这一层时，调用方只能“收下一帧就当成自己的响应”，双向主动请求会互相打架。
有了 `mini9p_peer_link` 后，链路上的帧会先按类型分流：

- 匹配本端当前 pending tag 的 R*，返回给本端请求者。
- 对端主动发来的 T*，转交给 `mini9p_server` 处理并回发响应。
- 其他外来 R*，交给可选观察器或直接忽略。

如果当前节点暂时没有本地 `request_handler`，`mini9p_peer_link` 仍然可以安全地区分帧类型；
只是对端主动发来的 T* 会收到自动生成的 `Rerror(ENOTSUP)`，而不是被误判成当前请求的响应。

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
config.ops = local_vfs_ops();
config.ops_ctx = &local_vfs;
m9p_server_init(&server, &config);
```

## Mini9P Service

核心文件：

- `mini9p_service.h`
- `mini9p_service.c`

service 是 STM32 上板联调用的薄组装层，内部持有静态实例：

- `local_vfs`
- `m9p_server`
- `m9p_peer_link`
- RX/TX frame buffer
- 默认 UART transport

对外只暴露两个入口：

```c
int mini9p_service_init(void);
int mini9p_service_poll_once(void);
```

初始化顺序：

```text
local_vfs_init
  -> m9p_server_init(local_vfs_ops)
  -> m9p_uart_transport_init_default
  -> m9p_peer_link_init(raw transport + server handler)
```

轮询流程：

```text
mini9p_service_poll_once
  -> m9p_peer_link_poll_once
  -> foreign T* -> m9p_server_handle_frame -> local_vfs
  -> foreign R* -> optional foreign_frame_handler
```

在 `PWOS_ENABLE_MINI9P_SERIAL` 开启时，`Core/Src/main.c` 使用该 service 作为主循环入口。此模式下 USART2 应由 Mini9P 二进制帧独占，不混入 VOFA 或其他文本日志。

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

当前 smoke test 目标是读取 `/sys/health` 并得到 `ok\n`。

## peer_link 的典型接法

如果某一侧既要作为 client 主动发请求，又要在等待自己的响应时承接对端请求，
推荐把 `m9p_peer_link_request` 直接挂给 `m9p_client`：

```c
struct m9p_peer_link link;
struct m9p_peer_link_config link_config;

m9p_peer_link_get_default_config(&link_config);
link_config.send_frame = raw_send_frame;
link_config.receive_frame = raw_receive_frame;
link_config.transport_ctx = raw_transport;
link_config.request_handler = m9p_server_handle_frame;
link_config.request_handler_ctx = &server;
link_config.dispatch_rx_buffer = rx_buf;
link_config.dispatch_rx_cap = sizeof(rx_buf);
link_config.dispatch_tx_buffer = tx_buf;
link_config.dispatch_tx_cap = sizeof(tx_buf);
m9p_peer_link_init(&link, &link_config);

m9p_client_init(&client, m9p_peer_link_request, &link);
```

这样在 `m9p_client_*` 调用内部等待 R* 的同时，`peer_link` 仍然会处理对端主动发来的 T*。

## peer_link Host 测试

当前仓库新增了独立 host 测试：

```text
pwos-shared/mini9p/test_peer_link_host.c
```

推荐本地编译命令：

```powershell
gcc -std=c11 -Wall -Wextra \
  -I. -I.\pwos-shared\mini9p \
  .\pwos-shared\mini9p\test_peer_link_host.c \
  .\pwos-shared\mini9p\mini9p_peer_link.c \
  .\pwos-shared\mini9p\mini9p_protocol.c \
  -o .\build\test_peer_link_host.exe
```
