# Mini9P Client / Server / Service

`pwos-shared/mini9p` 现在只保留 Mini9P 协议本体和节点侧组装层。旧的 `mini9p_peer_link` 已移除，
当前主从双向收发由 mesh runtime 负责分流和等待响应。

## 模块边界

```text
mini9p_protocol  帧编解码、T* 解析、R* 构造
mini9p_client    请求/响应式客户端，transport 由外层注入
mini9p_server    会话状态、fid 生命周期、请求分发、Rerror
mesh_node_service   节点侧组装层，串起已注入 backend + server + mesh_node_runtime + mesh_uart_transport
```

`mini9p_server` 不直接访问 littlefs 或设备资源。它只通过 `m9p_server_ops` 调用 backend。
`mesh_node_service` 同样不构造具体 backend；板级代码负责初始化 lfs/node_vfs 等本地资源后注入。

`mini9p_client` 也不直接理解 UART 或 mesh。它只要求外层提供一个 `m9p_transport_fn`：

- 主机侧当前由 `mesh_host_runtime_client_request()` 实现该回调；
- 该回调会把 Mini9P 请求封成 `MESH_TYPE_MINI9P` 帧，经 `mesh_uart_transport` 发出；
- 在等待本次 `R*` 响应期间，runtime 仍会继续处理途中插入的 REGISTER、LINK_STATE、ROUTE_UPDATE 等 mesh 帧。

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

- `mesh_node_service.h`
- `mesh_node_service.c`

service 是 STM32 节点侧的 mesh 装配层，内部持有静态实例：

- 一组 `mesh_uart_transport`
- `mesh_node_runtime`

具体 backend 由 `node_vfs_init()` 内部初始化并挂载；`mesh_node_mini9p_init` 持有 `m9p_server` 并只把 `node_vfs_ops()` 接入 Mini9P server。

对外暴露入口：

```c
struct mesh_node_service_port_config {
    bool enabled;
    uint8_t neighbor_addr;
    struct mesh_uart_transport_config uart_config;
};

struct mesh_node_service_config {
    mesh_processer_mini9p_server_handler_fn mini9p_server_handler;
    void *mini9p_server_ctx;
    size_t port_count;
    struct mesh_node_service_port_config ports[MESH_NODE_SERVICE_MAX_PORTS];
};

void mesh_node_service_get_default_config(struct mesh_node_service_config *out_config);
int mesh_node_service_init(const struct mesh_node_service_config *config);
void mesh_node_service_deinit(void);
int mesh_node_service_notify_link_up(void);
int mesh_node_service_poll_once(void);
```

初始化顺序：

```text
node_vfs_init initializes sys/dev/lfs
  -> m9p_server_init(node_vfs_ops)
  -> mesh_node_service_init(config with m9p_server_handle_frame + server ctx)
  -> mesh_uart_transport_init
  -> mesh_node_runtime_init(send_frame/receive_frame + injected Mini9P handler)
  -> auto REGISTER
```

轮询流程：

```text
mesh_node_service_poll_once
  -> mesh_node_runtime_poll_once
  -> mesh_uart_transport_receive_frame
  -> mesh_processer_process_frame
  -> local T* -> m9p_server_handle_frame -> injected backend
  -> local R* -> 封回 mesh MINI9P frame 发出
```

在节点侧，`mesh_node_service_init(config)` 会给 `mesh_node_runtime` 配好 `local_uid`、`boot_nonce`、REGISTER 能力位和 Mini9P handler；
`mesh_node_service_notify_link_up()` 可在未来链路明确恢复后再次主动发 REGISTER。

## PC 测试

server 单元测试：

```bash
cmake -S pwos-slave/User/uart_transport/test -B pwos-slave/User/uart_transport/test/build
cmake --build pwos-slave/User/uart_transport/test/build
pwos-slave/User/uart_transport/test/build/mini9p_server_test
```

上板串口联调工具见：

```text
tools/pc_master_emulator/
```

当前 smoke test 目标是读取 `/sys/health`，确认 UART -> mesh -> Mini9P -> injected backend 链路已打通。

## 当前职责边界

- `mini9p_client` 只负责构造请求、校验 tag/type 并解析 `R*`。
- `mesh_host_runtime` 负责把客户端请求封成 mesh 数据面帧，并在等待期间继续跑控制面。
- `mesh_node_runtime` 负责节点侧 REGISTER、自身地址同步、收帧分流和本地 Mini9P server 调用。
