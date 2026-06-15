# 未分配节点可由已分配节点中继 bootstrap 注册

## 状态

accepted

## 上下文

在多从机串联拓扑中，存在“下游从机无法与主机直接 UART 相连”的场景：

```text
Host <-> Slave A <-> Slave B
```

Slave B 上电时没有 mesh 地址（`0xFF`），它需要向主机发送 `REGISTER` 并收到 `ASSIGN`。但 Slave B 发出的广播 `REGISTER` 只能到达 Slave A，无法直接到达主机。

## 决策

允许已分配地址的从机中继下游未分配节点的 bootstrap 帧：

- `REGISTER(src=0xFF, dst=0xFF)` 可被已分配地址的 Slave A 接收并转发到其上游端口。
- Slave A 记录 `uid + boot_nonce -> ingress_port` 的临时映射。
- 主机下发的 `ASSIGN` 沿原路返回：Slave A 根据 pending 表把 `ASSIGN` 从记录过的下游端口发出。
- Slave A 在 ASSIGN 回转后学习 `node_addr -> downstream_port`，并向上游上报 `LINK_STATE`。

## 理由

1. **支持多跳拓扑**：主机不需要为每个从机提供独立物理接口，从机可以通过邻居链式接入。
2. **不破坏地址分配语义**：地址仍由主机统一分配，UID 仍是节点身份主键。
3. **最小化 relay 复杂度**：relay 只负责转发 `REGISTER`/`ASSIGN`，不代替主机做地址决策。
4. **链路有向性保持**： relay 上报的 `LINK_STATE(src=A, neighbor=B)` 只记录 A 看到的单向事实；主机需要收到双方 `LINK_STATE` 才会认为双向可达。

## 实现要点

- pending 表项：`used, uid[8], boot_nonce, ingress_port`，上限 `MESH_NODE_RUNTIME_MAX_BOOTSTRAP_PENDING`（当前 4）。
- 只对 `src=0xFF, dst=0xFF` 的 `REGISTER` 做中继；已分配地址的 `REGISTER` 正常处理。
- `ASSIGN` 回转时严格按 UID 匹配 pending，避免把主机给 Slave A 自己的 ASSIGN 误发给下游。

## 影响

- 从机 runtime 需要维护少量 pending 状态。
- 主机 ASSIGN 控制器必须对同一 UID 幂等，因为 REGISTER 重传会触发多次回转。
- 拓扑收敛依赖双方 `LINK_STATE` 上报；只有 `A->B` 没有 `B->A` 时，主机不会生成反向路由。

## 相关文件

- `pwos-slave/User/mesh/mesh_node_runtime.c`
- `pwos-shared/mesh/envelope/mesh_protocal.h`
- `docs/slave_mesh_runtime.md`
