# 控制面采用独立 link envelope

**状态**：accepted，已由 link frame v2 实现

动态注册、节点命名、拓扑维护、路由更新和多跳转发统一通过 mini9P 外层的 link
frame 表达；mini9P 本身继续只承载目标节点本地文件树访问语义。实现位置已经从旧
`pwos-shared/mesh` 迁移到 `pwos-shared/link` 和 `pwos-shared/mesh2`。

**考虑过的方案**：复用 `Rattach` 返回注册信息、新增 mini9P 控制消息类型、在 mini9P 外层增加 mesh envelope。前两者短期实现较少，但会混淆文件服务会话和网络加入流程；最终选择 mesh envelope，以保持控制面、转发层和 mini9P 文件语义解耦。
