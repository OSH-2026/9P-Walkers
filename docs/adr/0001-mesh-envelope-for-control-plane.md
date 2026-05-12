# 控制面采用 mesh envelope

动态注册、节点命名、拓扑维护、路由更新和多跳转发统一通过 mini9P 外层的 mesh envelope 表达；mini9P 本身继续只承载目标节点本地文件树访问语义。这样中间节点转发时只需要读取 envelope 中的路由信息，不需要理解 `Twalk`、`Tread` 等文件操作，也避免把注册/路由语义塞进 `Rattach.qid` 或新增 mini9P 文件协议消息类型。

**状态**：accepted

**考虑过的方案**：复用 `Rattach` 返回注册信息、新增 mini9P 控制消息类型、在 mini9P 外层增加 mesh envelope。前两者短期实现较少，但会混淆文件服务会话和网络加入流程；最终选择 mesh envelope，以保持控制面、转发层和 mini9P 文件语义解耦。
