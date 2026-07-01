# TCP/9000 WiFi mesh 透传

## 状态

superseded

## 历史决策

早期方案计划让 P4 监听 TCP/9000，把 STM32 mesh envelope 原样透传给外部 WiFi
模块，并把该连接伪装成一个 UART 入口。

## 废弃原因

- 它把 STM32 路由平面延伸到 IP 网络，模糊 owner host 的边界。
- 单连接地址学习和 bootstrap 广播难以支持可靠多主机。
- P4、S3 已经需要独立的主机发现、选主和跨主机服务协议。

## 替代方案

- P4 使用 Ethernet，S3 使用 WiFi，二者进入主机间 IP 平面。
- 主机通过 mDNS `_pwos._tcp` 和 TCP/9909 CBOR host RPC 通信。
- 每台主机只终止和管理自己的 STM32 UART 子树。
- 跨主机访问由 owner host 代理，不透传 link frame。

现行设计见 `docs/host_network.md`。旧 `mesh_wifi_link` 实现和专用文档已经删除。
