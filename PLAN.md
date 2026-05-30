# mesh_node ingress-port bootstrap plan

当前分支基线观察：

- `git status --short` 已执行，当前有 `TODO.md` 修改、`PLAN.md` 未跟踪、`image.png` 未跟踪。
- 新 base 中 `mesh_processer_receive_frame_fn` typedef 已经是 5 参数，包含 `uint8_t *out_ingress_port`。
- `mesh_processer_poll_once()` 已经调用 receive callback 并拿到 `ingress_port`，但目前没有继续把该端口传入 frame processing。
- 主机 `mesh_host_service_receive_frame()` 已经返回实际 UART ingress port。
- 从机 `mesh_node_service_receive_frame()` 仍是旧 4 参数实现，且从机 service config/runtime 内仍存在静态 `neighbor_addr` 路由假设。
- 从机 runtime 当前只有 `mesh_node_runtime_process_frame(runtime, frame, len)`，没有显式 ingress-port 入口。
- 主机 runtime 已有 `LINK_STATE` 处理：调用 shared cluster control handler、补 host-local link、刷新 VFS connectivity、同步 runtime slots。仍需核对 route table 下发闭环是否已存在。
- 本计划不引入从机 Wi-Fi，不引入 `mesh_wifi`、`wifi_supported` runtime 配置、虚拟 Wi-Fi port；不恢复旧分支静态 `neighbor_addr` 配置。

## Phase 1: ingress port API plumbing

目标：让收包路径完整携带真实入口端口，但不改变 bootstrap 行为。

1. 更新 shared processor API：
   - 新增 `mesh_processer_process_frame_from_port(processor, frame, len, ingress_port)`，作为带端口的主入口。
   - 保留 `mesh_processer_process_frame()`，内部调用 `_from_port(..., MESH_PROCESSER_INGRESS_PORT_NONE)`，降低既有调用点改动面。
   - `mesh_processer_poll_once()` 在 receive callback 得到 `ingress_port` 后调用 `_from_port()`。

2. 更新节点 runtime API：
   - 新增或恢复 `mesh_node_runtime_process_frame_from_port(runtime, frame, len, ingress_port)`。
   - 保留 `mesh_node_runtime_process_frame()` wrapper。
   - `mesh_node_runtime_poll_once()` 调用 runtime receive callback 的 5 参数版本，并把 ingress port 传给 `_from_port()`。

3. 更新从机 service receive path：
   - `mesh_node_service_receive_frame()` 改成 `mesh_processer_receive_frame_fn` 兼容签名，返回实际 UART port index。
   - 不在从机 service 中引入 Wi-Fi port 或虚拟 port。

4. 更新 PC tests/fakes：
   - 所有 fake receive callback 使用 5 参数签名。
   - 只断言 ingress port 行为相关事实，不依赖旧的 `tx_count` 固定下标作为核心语义。

验收：

- shared processor、node runtime、host runtime/service 相关测试可以编译。
- 从机 service 编译不再出现 receive callback 签名不匹配。

## Phase 2: remove slave static neighbor routing assumption

目标：从机 `addr -> port` 成为运行时学习结果，而不是静态配置。

1. 收敛从机 service port config：
   - 从 `pwos-slave/User/mesh/mesh_node_service.h/.c` 移除从机侧 `neighbor_addr` 配置字段、重复 neighbor 校验、按静态 neighbor 查 port 的逻辑。
   - 保留多 UART port enable/transport 初始化/round-robin receive。

2. 明确从机 send selector 语义：
   - `mesh_node_service_send_frame(ctx, next_hop, frame, len)` 中，将 `next_hop` 解释为运行时 selector/port id。
   - `MESH_NODE_SERVICE_NEIGHBOR_ANY` 仅作为 broadcast/all-ports selector，用于初始 REGISTER 或需要泛洪到所有已启用 UART 的场景。

3. 从机 direct-table 路由语义调整：
   - runtime 学到下游地址后写 `cluster_add_route(dst=node_addr, next_hop=ingress_port, metric=1)`。
   - 不再写 `dst -> dst` 作为多端口从机上的默认直连路由。

验收：

- 从机 service config 不再要求或暴露静态 `neighbor_addr`。
- 多端口发送由 runtime 学到的 port selector 决定。

## Phase 3: slave relay pending REGISTER and ASSIGN turnback

目标：实现 `ingress port -> pending REGISTER -> ASSIGN 回转 -> 学习下游路由`。

1. 在 `mesh_node_runtime` 增加 relay bootstrap 状态：
   - pending 表项：`used, uid[MESH_UID_LEN], boot_nonce, ingress_port`。
   - 表大小使用小固定数组，优先匹配当前 `MESH_NODE_SERVICE_MAX_PORTS` 或一个 runtime-local 小上限。

2. 处理下游 bootstrap REGISTER：
   - 在 `mesh_node_runtime_process_frame_from_port()` 中识别 `MESH_TYPE_REGISTER` 且 `src=0xff, dst=0xff`。
   - parse REGISTER。
   - 如果 ingress port 是已知实际端口，记录/覆盖 pending `uid + boot_nonce -> ingress_port`。
   - 将原始 REGISTER 转发到上游 control-plane port。

3. 记录本机上游 control-plane port：
   - 本机收到命中自己 UID 的 ASSIGN 后，更新本机 mesh addr。
   - 同时记录 ASSIGN 的 ingress port 为 upstream/control-plane port。
   - 后续本机 REGISTER/LINK_STATE 上报优先发往该 upstream port；未分配前仍可按现有 bootstrap selector 发送。

4. 处理主机 ASSIGN 回转：
   - 收到 `MESH_TYPE_ASSIGN` 时 parse ASSIGN。
   - 如果 ASSIGN UID 命中 pending 表：
     - 原始 ASSIGN 转发回 pending 的下游 ingress port。
     - 学习 `payload.node_addr -> pending.ingress_port`。
     - 在本地 direct route 写 `dst=payload.node_addr, next_hop/selector=pending.ingress_port`。
     - 构造并向主机上报 `LINK_STATE(src=本机, dst=host, neighbor=payload.node_addr, local_port=ingress_port)`。
     - 清理 pending。
   - 如果 ASSIGN UID 命中本机 UID，走本机 ASSIGN 逻辑，不误转发给下游。

5. LINK_STATE payload 字段核对：
   - 当前 `mesh_link_state_payload` 是固定 3 字节，现有字段包含 neighbor/link_up/quality。
   - 如果没有 `local_port` 字段，最小闭环先按“neighbor=下游地址，quality=ingress_port 或既有字段承载策略”实现，并同步更新协议注释/测试。
   - 如果决定扩展 payload，必须同步 `mesh_build_link_state()`、`mesh_parse_link_state()`、协议测试、host runtime 解析和兼容策略。

验收：

- 从机 relay 收下游 REGISTER 后不会把下游地址静态写死。
- 主机 ASSIGN 能沿 pending ingress port 回到下游。
- relay 学到下游地址对应端口，并向上游上报 LINK_STATE。

## Phase 4: host controller route distribution check/minimal patch

目标：保持主机侧“控制器下发指定路由”，不实现邻居传播路由模型。

1. 核对现有主机 LINK_STATE 处理：
   - `mesh_host_runtime_control_handler()` 是否已把 `src -> neighbor` 写入 topology。
   - `cluster_config_refresh_all_nodes_connectivity()` 是否足够触发 VFS 可达性更新。
   - 是否存在现成 route sync/ROUTE_UPDATE 下发逻辑。

2. 如果已有 route sync：
   - 只确保从机上报的 LINK_STATE 能进入 topology 并触发现有 route sync。
   - 不新增邻居传播协议。

3. 如果没有 route sync：
   - 补最小 controller-side 路由同步：host 从 cluster topology 计算到各节点 next hop。
   - 对每个已知从机下发指定 `ROUTE_UPDATE(dst, next_hop, metric, version)`。
   - 只由主机下发，relay/node 只落地 ROUTE_UPDATE，不互相传播路由。

4. 明确 host service ingress port：
   - 主机 service 已能返回 ingress port；如 route sync 需要 host 端口 selector，保持主机现有静态 `neighbor_addr` 机制或最小映射，不把该机制复制到从机。

验收：

- LINK_STATE 后 cluster topology 可见 `relay -> child`。
- 主机能给 relay/相关节点下发指定路由，relay 通过 direct table 按 port selector 转发到 child。

## Phase 5: tests

目标：用 PC 单测覆盖最小闭环，避免依赖旧分支 diff。

1. 更新 shared processor tests：
   - receive callback 输出 ingress port。
   - `poll_once()` 将 ingress port 传到新的 `_from_port()` 路径。

2. 更新 node runtime PC tests：
   - 本机 ASSIGN 从某个 ingress port 到达后，runtime 记录 upstream port。
   - 下游 REGISTER 从 port N 到达后，pending 表记录 `uid + boot_nonce -> N`。
   - 主机 ASSIGN 到达后，原始 ASSIGN 从 port N 发出。
   - 学习 `payload.node_addr -> port N`，mini9P/后续转发使用该 selector。
   - 上报 LINK_STATE 到 upstream port。

3. 更新 host runtime/service tests：
   - LINK_STATE from relay about child 后，topology/connectivity/route sync 行为符合现有 controller 模型。
   - 若新增 route sync，下发帧是主机生成的 ROUTE_UPDATE，不是节点间邻居传播。

4. 测试断言风格：
   - 不依赖旧 `tx_count` 固定下标来表达主行为。
   - 通过解析发出的 mesh frame type/src/dst/payload 和 send selector 来断言。

验收命令：

```sh
cmake --build pwos-slave/build/Debug
cmake --build tools/pc_master_emulator/build
cmake --build pwos-master-esp32p4/vfs_bridge/test/build
pwos-master-esp32p4/vfs_bridge/test/build/mesh_host_service_test
pwos-master-esp32p4/vfs_bridge/test/build/mesh_host_runtime_test
pwos-master-esp32p4/vfs_bridge/test/build/cluster_vfs_test
```

如果当前 build tree 中存在 node runtime PC 测试目标，也同步运行；若没有，至少保证源文件测试更新后可被现有手工/CI 构建方式编译。
