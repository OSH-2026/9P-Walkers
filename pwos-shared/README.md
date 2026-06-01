# pwos-shared

`pwos-shared` 目录存放主控 (`pwos-master-esp32p4`) 和从机 (`pwos-slave` / `pwos-slave-stm32f411`) 共同依赖的协议与运行时源码。原则上:

- **协议与运行时** 在此目录实现,跨板级复用。
- **板级 HAL、main、driver** 不放在这里,留在各自的 MCU 目录。
- **CMake / ESP-IDF 组件** 通过 `pwos-shared/<module>` 路径直接拉取源文件参与构建。

代码通用约定(均来自 `AGENTS.md` 与各头文件):

- C11(`-std=gnu11`),**不在热路径用 `malloc`**,所有 buffer、路由表、fid 表都是静态或栈上固定大小。
- 错误码:成功返回 `0`,失败返回负的 `MESH_ERR_*` / `M9P_ERR_*`。
- 公开回调通过函数指针注入,上下文以 `void *ctx` 透传;不强制要求调用方提供 C++ 兼容布局。
- 头文件使用 `#ifndef` include guard,命名形如 `MESH_PROCESSER_H`、`MINI9P_SERVER_H` 等。
- 命名:模块前缀(`m9p_`、`mesh_processer_`、`cluster_`、`mesh_uart_transport_`)。

## 目录结构

```text
pwos-shared/
├── mini9p/          # Mini9P 协议本体 + client/server
├── mesh/            # mesh envelope + 帧分发 + 拓扑/路由 + UART 适配
├── vfs/             # 预留:主机侧 dev/sys 抽离位(当前仅占位说明)
└── RUN_TIME_NODE_MESH.md  # 主机侧节点 mesh runtime 概要
```

三个子目录职责清晰且互不重叠:

| 子目录 | 角色 | 关键 API 前缀 |
|---|---|---|
| `mini9p/` | Mini9P v1 协议的帧编解码、client、server,以及 PC 端 client 单测 | `m9p_` |
| `mesh/` | mesh envelope、帧分发、拓扑/路由、UART transport | `mesh_` / `cluster_` |
| `vfs/` | 当前仅 README 占位:规划把 dev/sys 等 VFS 抽到此处 | — |

## 各子目录入口

### `pwos-shared/mini9p`

Mini9P v1 协议实现,负责帧编解码、client、server,以及 slave 侧的串口协议栈。详见 [`mini9p/README.md`](mini9p/README.md)。

源码:

- `mini9p_protocol.h/.c` — 帧编解码、错误码、qid/stat/dirent、`m9p_build_*` / `m9p_parse_*`。
- `mini9p_client.h/.c` — 客户端 attach/walk/open/read/write/stat/clunk,通过 `m9p_transport_fn` 注入底层。
- `mini9p_server.h/.c` — 服务端会话、fid 表、T* 分发、Rerror 编码,统一通过 `m9p_server_ops` 回调访问 backend。

帧格式(Magic `0x39 0x50`、Version `0x01`、CRC-16/CCITT-FALSE)见 `pwos-shared/mini9p/mini9p_protocol.h` 第 8–71 行;帧类型枚举 `enum m9p_type` 见同文件第 32–48 行。

PC 测试:

```bash
gcc -std=c11 -Wall -Wextra \
  -I pwos-shared/mini9p \
  pwos-shared/mini9p/test_mini9p_client_host.c \
  pwos-shared/mini9p/mini9p_client.c \
  pwos-shared/mini9p/mini9p_protocol.c \
  -o /tmp/test_mini9p_client_host && /tmp/test_mini9p_client_host
```

server 的 host test 与 `mini9p_server_test` 可执行文件留在构建目录中(`pwos-shared/mini9p/test/build/`)。

### `pwos-shared/mesh`

mesh 层是 MCU 集群的"信封 + 拓扑 + 适配"三件套,被主控和从机共同使用。详见 [`mesh/README.md`](mesh/README.md)。

源码分四个子模块:

- `mesh/envelope/mesh_protocal.h/.c` — 帧格式、CRC、控制面编解码。`enum mesh_type` 见同头文件第 57–70 行,含 `MESH_TYPE_MINI9P=0x01`、`MESH_TYPE_REGISTER=0x10`、`MESH_TYPE_ASSIGN=0x11`、`MESH_TYPE_PING=0x12`、`MESH_TYPE_PONG=0x13`、`MESH_TYPE_TIME_SYNC=0x14`、`MESH_TYPE_ROUTE_UPDATE=0x15`、`MESH_TYPE_LINK_STATE=0x16`、`MESH_TYPE_NEIGHBOR_PROBE_REQUEST=0x17`、`MESH_TYPE_NEIGHBOR_PROBE_RESPONSE=0x18`、`MESH_TYPE_ERROR=0x7F`。
- `mesh/processer/mesh_processer.h/.c` — 帧分发中间层。提供 `send_frame / receive_frame / route_lookup` 三条必填回调,以及 `control_handler / mini9p_server_handler / mini9p_client_handler` 三条可选回调(`mesh_processer.h` 第 70–176 行)。
- `mesh/cluster/cluster.h/.c` — 拓扑/路由管理层。`enum cluster_mode` 第 41–44 行;提供 `DIRECT_TABLE` / `TOPOLOGY` 两种工作模式;以及与 processor 同签名的适配函数 `cluster_processor_route_lookup` / `cluster_processor_control_handler`(第 240–264 行)。
- `mesh/transport/mesh_uart_transport.h/.c` — UART 适配层,通过宏 `MESH_UART_TRANSPORT_USE_STM32_HAL` 或 `ESP_PLATFORM` 切换 STM32 HAL / ESP-IDF / POSIX 后端,默认 POSIX。

PC 单测在 `pwos-shared/mesh/module test(on PC)/`:

```bash
gcc -std=c11 -Wall -Wextra \
  -I pwos-shared/mesh/envelope -I pwos-shared/mesh/cluster \
  -I pwos-shared/mesh/processer -I pwos-shared/mini9p \
  -I pwos-slave/User/mesh \
  "pwos-shared/mesh/module test(on PC)/test_mesh_node_runtime_host.c" \
  pwos-shared/mesh/envelope/mesh_protocal.c \
  pwos-shared/mesh/cluster/cluster.c \
  pwos-shared/mesh/processer/mesh_processer.c \
  pwos-shared/mini9p/mini9p_protocol.c \
  pwos-slave/User/mesh/mesh_node_runtime.c \
  -o /tmp/test_mesh_node_runtime_host && /tmp/test_mesh_node_runtime_host
```

详细 spec 文档:

- `pwos-shared/mesh/envelope/mesh_protocol_spec.md`
- `pwos-shared/mesh/processer/mesh_processer_spec.md`
- `pwos-shared/mesh/cluster/mesh_cluster_spec.md`
- `pwos-shared/mesh/mesh_overview_spec.md`(接口边界)
- `pwos-shared/mesh/mesh_host_vfs_spec.md`(主机侧 VFS 桥接语义)

### `pwos-shared/vfs`

当前 `vfs/README.md` 只写了一句占位:

> 现在 dev/sys 这些 vfs 主要在从机侧。如果后面主机侧也要做这些,考虑迁移到此文件夹。

也就是说,`vfs/` 是为未来把主机侧 VFS 抽象抽出来预留的位置,**当前没有任何 .c/.h 源文件**,只承担说明作用。

## 模块之间的依赖关系

```text
pwos-master-esp32p4 / pwos-slave
        |
        |  CMakeLists.txt / main/CMakeLists.txt 直接包含
        v
   pwos-shared
        |
        |---- mini9p                (独立,不依赖 mesh)
        |
        |---- mesh
        |      |
        |      |---- envelope       (仅依赖 stdint,无内部依赖)
        |      |---- processer      (依赖 envelope + mini9p_protocol)
        |      |---- cluster        (依赖 envelope)
        |      |---- transport      (依赖 envelope,按宏切换 HAL/ESP/POSIX)
        |
        |---- vfs                  (占位,无源码)
```

具体交叉引用(在头文件中显式 `#include`):

- `mesh/processer/mesh_processer.h` 第 8–9 行:同时 include `../envelope/mesh_protocal.h` 和 `../../mini9p/mini9p_protocol.h`,因此 processor 的 mini9P server/client 回调类型直接使用 `m9p_frame_view`。
- `mesh/cluster/cluster.h` 第 8 行:include `../envelope/mesh_protocal.h`,在 `cluster_apply_route_update()` 中直接使用 `struct mesh_route_update_payload`。
- `mesh/transport/mesh_uart_transport.h` 第 8 行:include `../envelope/mesh_protocal.h`,内部对 mesh 帧做透明转发。

`mini9p/` 自身不依赖 `mesh/`,因此可以独立做 PC 单测;反过来,`mesh/` 也不会反向依赖 `mini9p_client` / `mini9p_server`,mini9P 处理仅以"原始 mini9P 帧字节流"的形式出现(`mesh_processer.h` 第 154–176 行)。

## 主控 / 从机如何引用本目录

### 主控 ESP32-P4

`pwos-master-esp32p4/main/CMakeLists.txt` 通过 `EMBED_FILES` 嵌入 `web/index.html`,并把 `pwos-shared/mini9p` 与 `pwos-shared/mesh` 编译进固件(见根 `README.md` 第 96–97 行、`pwos-master-esp32p4/README.md`)。

主控侧典型接线(详见 `RUN_TIME_NODE_MESH.md`):

1. 启动时 `mesh_host_service_start_default_task()`。
2. 默认 runtime 自动初始化 cluster、`cluster_vfs`、UART transport、后台 poll 任务。
3. bootstrap REGISTER 触发主机分配地址；ASSIGN 成功发出后或收到已分配地址 REGISTER 时，`mesh_host_runtime_register_assigned_node()` 调用 `cluster_config_on_mesh_node_registered()`，同步 online 位、VFS UID 映射和 mesh-backed Mini9P client。
4. 收到 LINK_STATE / ROUTE_UPDATE → `cluster_config_refresh_all_nodes_connectivity()` → 批量刷新真实可达性；REGISTER 本身不再伪造 host 直连边。
5. 上层通过 `cluster_vfs_attach("mcu1")` 与 `cluster_vfs_read_path("/mcu1/sys/health", ...)` 访问。
6. client 请求被封成 `MESH_TYPE_MINI9P` 帧,经 `mesh_host_runtime_client_request()` 注入的 transport 发出；等待 `R*` 期间 runtime 独占 dispatch，并继续处理途中到达的 REGISTER、LINK_STATE 等控制帧。

### 从机 STM32

`pwos-slave/User/app/mesh_node_mini9p_init.c` 与 `pwos-slave/User/mesh/mesh_node_service.c` 通过 CMake 复用本目录源码。初始化顺序(详见 `mini9p/README.md`):

```text
node_vfs_init()            // sys/dev/lfs
   -> m9p_server_init(node_vfs_ops)
   -> mesh_node_service_init(config with m9p_server_handle_frame + server ctx)
   -> mesh_uart_transport_init
   -> mesh_node_runtime_init
   -> auto REGISTER
```

`PWOS_ENABLE_MINI9P_SERIAL` 打开时,`Core/Src/main.c` 切换为 Mini9P 串口联调模式:USART2 只承载 Mini9P 二进制帧,不混入 VOFA 文本(详见 `pwos-slave/README.md`)。

## 跨模块约束(由代码与 spec 总结)

1. **mini9P 是 mesh 的 payload**:中继节点只看 mesh envelope 头,不解析 mini9P;只有 `dst == local_addr` 时才进入 mini9P 分流(`mesh_overview_spec.md` 第 53–58 行)。
2. **mesh 地址可重分配、UID 不可变**:UID (`MESH_UID_LEN = 8`) 是节点身份主键,主机按 UID 分配 `mcuN` 名字而非按短地址(`mesh_overview_spec.md` 第 268–274 行)。
3. **拓扑有向**:`A → B` 不自动补 `B → A`;反向路径必须由 B 自己上报 `LINK_STATE`(`mesh_overview_spec.md` 第 192–196 行)。
4. **processor 只做"问答式查路由"**:依赖 `route_lookup(dst, &next_hop, &is_local)`,不维护路由真相(`mesh_processer.h` 第 100–117 行)。
5. **mini9P server 不知道 mesh**:server 的唯一入口是 `m9p_server_handle_frame()`,对 backend 的访问完全走 `m9p_server_ops`(`mini9p_server.h` 第 67–139 行)。
6. **mini9P client 不知道 mesh**:client 通过 `m9p_transport_fn` 把"发什么、收什么"完全交给外层,主控的 `mesh_host_runtime_client_request()` 实现了这一回调(`mini9p_client.h` 第 17–40 行)。
7. **没有动态内存**:静态路由表 `CLUSTER_MAX_ROUTES = 16`、`CLUSTER_MAX_NODES = 16`、`CLUSTER_MAX_LINKS = 32`(`cluster.h` 第 32–34 行);server fid 表 `M9P_SERVER_MAX_FIDS = 16`(`mini9p_server.h` 第 29 行)。

## 推荐使用方式

### 新增协议字段

1. 修改 `pwos-shared/mini9p/mini9p_protocol.h` 的 frame / type / struct 定义。
2. 在 `pwos-shared/mesh/envelope/mesh_protocal.h` 看是否需要新 mesh 消息类型。
3. 同步更新 `pwos-shared/mini9p/test_mini9p_client_host.c` 或 `pwos-shared/mesh/module test(on PC)/` 下的 PC 单测。
4. 主机侧 `pwos-master-esp32p4/vfs_bridge/` 和从机侧 `pwos-slave/User/backend/` 的 backend 实现按需更新。

### 调试链路问题

按 `mesh/README.md` 的"接入顺序建议":

1. 先接 `send_frame / receive_frame / route_lookup`,确保可收发完整 mesh 帧。
2. 接 `control_handler`,让 REGISTER / ASSIGN / ROUTE_UPDATE 生效。
3. 接 `mini9p_server_handler / mini9p_client_handler`,打通文件请求链路。

## 进一步阅读

- `RUN_TIME_NODE_MESH.md` — 主机侧 runtime 链条与对外接口分层。
- `mini9p/README.md` / `mini9p/server_plan.md` — Mini9P server 当前计划与状态。
- `mesh/README.md` / `mesh/mesh_overview_spec.md` — mesh 模块边界与接入顺序。
- `mesh/mesh_host_vfs_spec.md` — 主机侧 VFS 桥接语义。
- 根目录 `README.md` 与 `AGENTS.md` — 仓库级约定与命令。
- 跨模块设计决策:`docs/adr/0001-mesh-envelope-for-control-plane.md` 与 `docs/adr/0002-master-owns-global-topology.md`。
