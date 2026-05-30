# TODO

## Mini9P 多 UART 与共享 backend 串行化

- 当前 `mesh_node_service` 默认安全前提是单 UART、单 `m9p_server`、单线程轮询访问 backend。
- 如果未来多个 UART/service 同时访问同一个 `node_vfs/lfs_vfs/dev_vfs`，需要引入 backend 串行化机制。
- 推荐方向：每条 UART 保持独立 `m9p_server/mesh_node_runtime/transport`，共享 backend 通过单 backend worker 队列串行处理。
- `mesh_node_service_backend` 当前可注入 worker proxy ops，但它本身不提供队列、锁、超时或取消语义。
- 后续可把 `uart` 从 `mesh_node_service_backend` 拆到 `mesh_node_service_config`，让 backend 注入和 transport 配置职责分离。

## MCU1 双 UART 中继缺失

- 当前硬件拓扑：PC -- `mcu1` USART2 @ 1000000；`mcu1` USART1 @ 115200 -- `mcu2` USART1 @ 115200。
- 现象：`mcu1` 单独上板正常，`mcu2` 单独上板正常；但在链式拓扑下 reset `mcu2` 后，PC 收不到 `mcu2` 的 REGISTER。
- 当前代码根因：`pwos-slave` 初始化了 USART1/USART2，但 `mesh_node_service` 只把 mesh service 绑定到 `huart2`；`huart1` 没有 mesh transport/runtime 轮询，因此不能接收或转发 `mcu2` 的帧。

如果快速跑通，考虑直接在
- 共享 `mesh_node_service` 目前也是单 `mesh_uart_transport` + 单 `mesh_node_runtime` 全局实例，不支持 mcu1 同时作为上游节点和下游中继。
- 后续实现方向：为 mcu1 增加双端口 mesh bridge/runtime，分别管理 USART2 上游和 USART1 下游；按端口轮询收帧，并在 `send_frame(next_hop, ...)` 中按下一跳选择输出 UART。
- 注意：当前 `mesh_processer` 会把 `dst=0xff` 的 bootstrap REGISTER 当作本机控制帧处理。中继模式下需要明确 mcu1 对下游 bootstrap REGISTER 的策略：转发到 PC、代理注册，或新增专门 bridge 逻辑。


# 目前从机主循环的uart还是轮询！

# 两个方案：
1、 考虑把runtime改成多端口
2、 考虑runtime单端口，service启动多个node runtime

 明天提示词可以改成这个版本：

  继续 mesh_node 工作。注意：之前分支的 base 有问题，现在已经切到另一个分支作为 main；这个新 base 本身没有从机 Wi-Fi，
  所以不要再做“清 Wi-Fi”的工作，也不要把旧分支里的 Wi-Fi 相关改动带回来。

  当前目标：
  在新 base 上重新实现“显式 ingress port + 从机多端口中继 bootstrap + 主机控制器下发指定路由”的最小闭环。

  要求：
  1. 先 `git status --short`，再搜索现有代码，不要假设旧分支改动还在。
  2. 不要套旧 diff。按新 base 的实际结构重新实现。
  3. 主机侧仍采用“控制器下发指定路由”，不要实现邻居传播路由模型。
  4. 从机侧不要加入 Wi-Fi，不要引入 mesh_wifi、wifi_supported runtime 配置、虚拟 Wi-Fi port。
  5. 不要恢复静态 `neighbor_addr` 配置；从机 `addr -> port` 是动态结果，不是静态配置。

  需要实现/检查的能力：
  1. `mesh_processer_receive_frame_fn` 显式带 `uint8_t *out_ingress_port`。
  2. `mesh_processer_poll_once()` 调用 receive callback 后拿到 ingress port。
  3. node service/runtime receive path 返回实际 UART ingress port。
  4. 增加或恢复 `mesh_node_runtime_process_frame_from_port(runtime, frame, len, ingress_port)`，`poll_once()` 使用它。
  5. 从机中继收到下游 bootstrap `REGISTER src=0xff dst=0xff`：
     - parse REGISTER；
     - pending 表记录 `uid + boot_nonce -> ingress_port`；
     - 转发原始 REGISTER 到上游控制面端口。
  6. 收到主机 `ASSIGN`：
     - parse ASSIGN；
     - 如果命中 pending UID，则把原始 ASSIGN 转发回 pending 的下游 ingress port；
     - 学习 `payload.node_addr -> ingress_port`；
     - 本地 direct route 记录为 `dst=payload.node_addr, next_hop/selector=ingress_port`；
     - 向主机上报 `LINK_STATE(src=本机, dst=host, neighbor=payload.node_addr, local_port=ingress_port)`；
     - 清 pending。
  7. 本机收到自己的 ASSIGN 后：
     - 更新本机 mesh addr；
     - 记录上游 control-plane port；
     - 可向上游重发 REGISTER / LINK_STATE，按新 base 现有设计收敛。
  8. 主机侧：
     - 确认已有 `LINK_STATE` 处理是否会更新 topology 并触发 route table 下发；
     - 如果已有，只接入从机上报即可；
     - 如果没有，补最小处理：`src -> neighbor + local_port` 进入 cluster topology，然后调用现有 route sync。
 
  验证：
  1. `cmake --build pwos-slave/build/Debug`
  2. `cmake --build tools/pc_master_emulator/build`
  3. `cmake --build pwos-master-esp32p4/vfs_bridge/test/build`
  4. 运行：
     - `pwos-master-esp32p4/vfs_bridge/test/build/mesh_host_service_test`
     - `pwos-master-esp32p4/vfs_bridge/test/build/mesh_host_runtime_test`
     - `pwos-master-esp32p4/vfs_bridge/test/build/cluster_vfs_test`
  5. 如果有 node runtime PC 测试，更新为按新 ingress-port 行为断言，不依赖旧 tx_count 下标。

  一句话版：新 base 没有 Wi-Fi，重点只做 ingress port -> pending REGISTER -> ASSIGN 回转 -> LINK_STATE 上报 -> 主机下发
  路由 这条闭环。



## 备选：邻居传播路由模型

- 当前路由策略先保持“控制器下发指定路由”：主机/控制器计算或指定 `dst -> next_hop -> metric`，节点只应用收到的路由更新。
- 后续可评估邻居传播模型：每个节点周期或触发式向直连邻居广播自己的可达表，接收方用“邻居宣告 metric + 1”与本地路由表比较，选择更优路由。
- 在该模型里，接收方的 `next_hop` 应该是宣告该路由的邻居地址，也就是入站控制帧的 `src`；不应直接信任 payload 里的 `next_hop`，否则会把邻居内部路径误当成本机下一跳。
- `addr -> port` 映射可以由入站端口学习，但应优先从直连邻居控制帧学习，不能简单用普通转发数据帧的原始 `src` 推断直连关系。
- 需要处理 count-to-infinity/环路收敛问题：metric 会增大但不代表立刻无环，需要设置最大 metric、路由老化、版本/序列号、split horizon 或 poison reverse。
- 如果以后实现，建议新增“route advertise”语义，避免和现有“控制器指定 route update”混用。

![alt text](image.png)