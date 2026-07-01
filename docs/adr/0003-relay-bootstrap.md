# 未分配节点可由已分配节点中继 bootstrap 注册

## 状态

accepted，已由 `node_control` 实现

## 上下文

在多从机串联拓扑中，存在“下游从机无法与主机直接 UART 相连”的场景：

```text
Host <-> Slave A <-> Slave B
```

Slave B 上电时没有 mesh 地址（`0xFF`），它需要向主机发送 REGISTER 并收到 ASSIGN。
但广播 REGISTER 只能到达 Slave A，无法直接到达主机。

## 决策

允许已分配地址的从机中继下游未分配节点的 bootstrap 帧：

- `CTRL_NODE_REGISTER(src=0xFF, dst=0xFF)` 可被已分配地址的 Slave A 接收并转发到其上游端口。
- Slave A 记录 `uid + boot_id -> ingress_port` 的临时映射。
- 主机下发的 `CTRL_ADDR_ASSIGN` 沿原路返回：Slave A 根据 pending 表从记录的下游端口发出。
- Slave A 在 ASSIGN 回转后学习下游关系，并向上游上报 `CTRL_LINK_STATE`。

## 理由

1. **支持多跳拓扑**：主机不需要为每个从机提供独立物理接口，从机可以通过邻居链式接入。
2. **不破坏地址分配语义**：地址仍由主机统一分配，UID 仍是节点身份主键。
3. **最小化 relay 复杂度**：relay 只负责转发 REGISTER/ASSIGN，不代替主机做地址决策。
4. **链路有向性保持**：relay 上报的 LINK_STATE 只记录已观察到的邻接事实。

## 实现要点

- pending 表按 `uid[3], boot_id, ingress_port` 保存，容量固定。
- 只对 `src=0xFF, dst=0xFF` 的 `CTRL_NODE_REGISTER` 做 bootstrap 中继。
- `ASSIGN` 回转时严格按 UID 匹配 pending，避免把主机给 Slave A 自己的 ASSIGN 误发给下游。

## 影响

- 从机 runtime 需要维护少量 pending 状态。
- 主机 ASSIGN 控制器必须对同一 UID 幂等，因为 REGISTER 重传会触发多次回转。
- 拓扑收敛依赖 link state 和 coordinator 的 route planner。

## 相关文件

- `pwos-slave/User/mesh2/node_control.c`
- `pwos-shared/link/pwos_link_frame.h`
- `pwos-shared/mesh2/pwos_mesh2_control.h`
- `docs/slave_mesh_runtime.md`
