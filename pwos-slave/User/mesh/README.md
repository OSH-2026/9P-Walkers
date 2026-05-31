# STM32 Slave Mesh

`pwos-slave/User/mesh` 是 STM32 从机侧 mesh 装配层。它把 shared mesh processor、direct-table cluster、raw UART transport 和本地 Mini9P server 串成一个可轮询服务。

## Files

- `mesh_node_service.h/.c`: 板级 service。拥有全局 service 实例、UART 端口数组、round-robin 收包、broadcast REGISTER、以及动态 `mesh_addr -> UART port` 映射。
- `mesh_node_runtime.h/.c`: 节点运行时。负责 REGISTER、ASSIGN、本机地址同步、direct-table 路由刷新、Mini9P server 接入和 ingress-port 感知处理。

## Layering

`mesh_node_service` 直接管理硬件 transport。它初始化每个启用的 UART 端口，轮询收帧，并把实际入口端口传给 runtime。发送时，service 接收 runtime/processor 给出的 `next_hop`，再查本地动态 addr-port 表选择具体 UART。

`mesh_node_runtime` 不直接访问 UART。它只通过 `mesh_processer_send_frame_fn` 和 `mesh_processer_receive_frame_fn` 回调收发完整 mesh 帧。runtime 内部维护 direct-table cluster，当前约定为：

- cluster route 的 `next_hop` 仍是 mesh 地址。
- service 的 addr-port 表负责把 mesh 地址解析为 UART port。
- `MESH_NODE_SERVICE_NEIGHBOR_ANY` 是 all-ports broadcast selector，主要用于 bootstrap REGISTER。

## Bootstrap Flow

1. 板级代码初始化 backend 和 Mini9P server。
2. 板级代码调用 `mesh_node_service_get_default_config()`，填入 UART handle、Mini9P handler/context 后调用 `mesh_node_service_init()`。
3. service 初始化 UART transport 和 runtime，并默认广播一帧 REGISTER。
4. 主机下发 ASSIGN 后，runtime 校验 UID，只接受命中本机 UID 的 ASSIGN。
5. ASSIGN 命中后，runtime 同步 `cluster.config.local_addr` 和 `processor.config.local_addr`，记录上游 control-plane port，然后向上游确认 REGISTER。
6. 本机 ASSIGN 完成后，runtime 向所有已启用 UART 端口广播 `NEIGHBOR_PROBE_REQUEST`，用端口级探测学习直连邻居的 `mesh_addr -> ingress_port`。
7. 若本机作为 relay 收到下游 bootstrap `REGISTER(src=0xff,dst=0xff)`，runtime 记录 pending `uid + boot_nonce -> ingress_port`，并把原始 REGISTER 转发到上游 control-plane port。主机 ASSIGN 到来后，runtime 会把原始 ASSIGN 回转到对应下游 ingress port，并学习 `child_addr -> port`。

## Receive And Learning

`mesh_node_service_poll_once()` 每次最多处理一帧。service 按 round-robin 顺序扫描启用端口；收到帧后设置 `out_ingress_port`，然后进入 runtime。

`mesh_node_runtime_process_frame_from_port()` 会先解码 mesh frame，并按以下顺序处理：

- `NEIGHBOR_PROBE_REQUEST`：若本机已有正式地址，从同一 ingress port 直接回 `NEIGHBOR_PROBE_RESPONSE`，不进入普通转发路径。
- `NEIGHBOR_PROBE_RESPONSE`：唯一允许建立 direct neighbor 的帧类型。runtime 在 direct-table cluster 中写入 `dst=src, next_hop=src`，并通过 `learn_peer_port` 回调通知 service 记录 `src -> ingress_port`。若本机已经知道上游 control-plane，也会立即向上游发送 `LINK_STATE(src=本机, neighbor=src)`。
- 下游 bootstrap `REGISTER(src=0xff,dst=0xff)`：若本机已注册且已知道上游 control-plane，则记录 pending 并把原始 REGISTER 转发上游。
- pending `ASSIGN`：按之前记录的 ingress port 回转到下游，并学习 `payload.node_addr -> ingress_port`，随后向上游发送 `LINK_STATE(src=本机, neighbor=payload.node_addr)`。
- 其他控制帧或数据帧：交给 shared mesh processor 做本机控制处理、Mini9P 分发或普通转发。

## Send Path

processor 需要发送或转发时，会先通过 cluster 查 `dst -> next_hop`。在从机 runtime 中，这个 `next_hop` 是 mesh 地址。service 收到 send callback 后：

- 如果 `next_hop == MESH_NODE_SERVICE_NEIGHBOR_ANY`，发送到所有已初始化 UART 端口。
- 否则在 addr-port 表中查 `next_hop -> port_id`，并从该 UART 发送。
- 如果没有映射，返回 `-MESH_ERR_NO_ROUTE`。

## Current Limits

- service 使用一个全局实例，不支持同时创建多个独立 service。
- addr-port 表容量等于 `MESH_NODE_SERVICE_MAX_PORTS`，适合当前小规模链式/星型实验拓扑。
- 当前 runtime 不会从普通 mesh frame 的 `src` 推断直连邻居；只有 `NEIGHBOR_PROBE_RESPONSE` 和 pending ASSIGN 回转会建立 `mesh_addr -> ingress_port`。
- 当前最小闭环依赖双方 `LINK_STATE` 分别上报各自看到的直连邻居事实；主机不会自动把 `A -> B` 补成 `B -> A`。

## Typical Board Integration

当前 `pwos-slave/User/app/mesh_node_mini9p_init.c` 使用默认单端口配置，将 `huart2` 接入 mesh service：

```c
mesh_node_service_get_default_config(&mesh_config);
mesh_config.ports[0].uart_config.uart = &huart2;
mesh_config.mini9p_server_handler = m9p_server_handle_frame;
mesh_config.mini9p_server_ctx = &g_mini9p_server;
return mesh_node_service_init(&mesh_config);
```

主循环中周期性调用：

```c
(void)mesh_node_service_poll_once();
```
