# Mini9P

`pwos-shared/mini9p` 只实现 mini9P 协议本体、client 和 server。它不直接依赖
UART、link、mesh2、FreeRTOS 或 ESP-IDF。

## 当前承载方式

mini9P 帧不会裸跑在线缆上，而是作为 `PWOS_LINK_TYPE_DATA_MINI9P` 的 payload：

```text
P4 cluster_vfs
  -> mini9p_client
  -> session_manager transport callback
  -> link DATA_MINI9P
  -> STM32 node_control 转发/投递
  -> service_runtime
  -> mini9p_server
  -> local_vfs
```

中继 STM32 只看 link frame 头部做转发，不解析 mini9P payload。

## 模块边界

- `mini9p_protocol.c/.h`：帧编解码、T* 构造/解析、R* 构造/解析、dirent/stat。
- `mini9p_client.c/.h`：同步请求式 client，通过 `m9p_transport_fn` 注入传输层。
- `mini9p_server.c/.h`：attach/fid/open/read/write/stat/clunk 状态机，通过 `m9p_server_ops` 注入 backend。

主机侧传输由 `pwos-master-esp32p4/host_sessions/session_manager.c` 实现。
从机侧服务由 `pwos-slave/User/service/service_runtime.c` 实现。

## PC 测试

```bash
gcc -std=c11 -Wall -Wextra \
  -I pwos-shared/mini9p \
  pwos-shared/mini9p/test_mini9p_client_host.c \
  pwos-shared/mini9p/mini9p_client.c \
  pwos-shared/mini9p/mini9p_protocol.c \
  -o /tmp/test_mini9p_client_host
/tmp/test_mini9p_client_host
```
