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
5. ASSIGN 命中后，runtime 同步 `cluster.config.local_addr` 和 `processor.config.local_addr`，随后重新发送 REGISTER。

## Receive And Learning

`mesh_node_service_poll_once()` 每次最多处理一帧。service 按 round-robin 顺序扫描启用端口；收到帧后设置 `out_ingress_port`，然后进入 runtime。

`mesh_node_runtime_process_frame_from_port()` 会先解码 mesh frame。若 `frame.src` 不是 `MESH_ADDR_UNASSIGNED`，runtime 会：

- 在 direct-table cluster 中写入 `dst=src, next_hop=src`。
- 通过 `learn_peer_port` 回调通知 service 记录 `src -> ingress_port`。
- 再把帧交给 shared mesh processor 进行本机控制帧处理、Mini9P 分发或转发。

## Send Path

processor 需要发送或转发时，会先通过 cluster 查 `dst -> next_hop`。在从机 runtime 中，这个 `next_hop` 是 mesh 地址。service 收到 send callback 后：

- 如果 `next_hop == MESH_NODE_SERVICE_NEIGHBOR_ANY`，发送到所有已初始化 UART 端口。
- 否则在 addr-port 表中查 `next_hop -> port_id`，并从该 UART 发送。
- 如果没有映射，返回 `-MESH_ERR_NO_ROUTE`。

## Current Limits

- service 使用一个全局实例，不支持同时创建多个独立 service。
- addr-port 表容量等于 `MESH_NODE_SERVICE_MAX_PORTS`，适合当前小规模链式/星型实验拓扑。
- 当前 runtime 通过普通入站帧学习 `src -> ingress_port`。在复杂多跳拓扑里，普通帧的 `src` 不一定是物理直连邻居，后续若引入完整中继控制面，应改为从 REGISTER/ASSIGN/LINK_STATE 或主机下发路由中学习更严格的端口映射。
- 当前代码还没有实现完整的下游 REGISTER pending 表、ASSIGN 回转和 LINK_STATE 上报闭环。

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
