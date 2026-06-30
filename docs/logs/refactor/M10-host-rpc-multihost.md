# M10 Host RPC 与多主机 Alpha

日期：2026-06-30  
基线：`db47ec0` 之后的工作树  
状态：软件实现和 PC/固件构建完成，等待双 P4 上板验收

## 范围

- 新增共享 `host_rpc` 协议：4 字节大端长度前缀加规范 CBOR map，单帧上限
  1280 字节、payload 上限 1024 字节；拒绝重复字段、非规范整数和不定长 CBOR。
- 方法包含 `host.advertise`、`topology.whoowns/sync`、`cluster.read_node` 和
  `cluster.write_node`。TCP server 使用 9909 端口，每个连接处理一个有 deadline 的
  请求。
- Ethernet MAC 派生稳定 `host_uid` 和 `pwos-xxxx` hostname；NVS 保存并在每次启动
  递增 epoch。主机按 `(epoch, priority, host_uid)` 的字典序选出 leader，peer 15 秒
  未刷新后过期。
- mDNS 发布并查询 `_pwos._tcp`。leader 汇总每个 owner 的节点快照，为 UID 分配全局
  `mcuN`；follower 接受 leader 整表并保留 `global_target -> owner_target` 映射。
- WebShell 的 `cat` 和 `echo/fault` 写入先查本机 coordinator，未命中后通过 host RPC
  转发。`ls /` 使用 leader 的全局名字，并处理 follower 本地别名重命名。
- `/host/sys/hosts` 和 `hosts` 命令输出本机角色、leader、peer、同步统计和节点 owner。
- mesh 新增 `CTRL_HOST_ADVERTISE`。STM32 记录直接可见 leader 的 UID、epoch、priority
  和入口端口，拒绝其他入口的 assign、lease ack 和 route update；authority 5 秒过期。
- 原 MCU `DATA_RPC` 客户端从 `host_rpc/` 移到 `slave_rpc/`，避免与 TCP 主机间 RPC
  混淆；旧文件已删除。

## 已执行验证

- 14 组 PC CTest 全部通过：link、mesh2、RPC、Job、CSC、host coordinator、host API、
  host sessions、host shell、host RPC protocol/endpoint、slave backend/compute/service。
- host RPC 测试覆盖 CBOR 帧、畸形输入、方法 payload、read/write endpoint、advertise、
  whoowns、topology sync、选主平局和 peer expiry。
- ESP32-P4 构建通过：`hello_world.bin` 为 `0x97cb0`，1 MiB app 分区剩余
  `0x68350`（41%）。
- STM32F407 Debug 全量 clean build 通过：RAM 75688 B / 128 KiB（57.75%），FLASH
  104308 B / 512 KiB（19.90%）。
- `git diff --check` 通过。

## 双主机上板验收

连接方式：P4-A 和 P4-B 接入同一 LAN；P4-A UART 接 MCU-A 子树，P4-B UART 接
MCU-B 子树。两个 P4 不能接到同一 UART 电气总线。

1. 两块 P4 和 STM32 都烧录本阶段固件，等待至少 20 秒。在两边 WebShell 执行：

```text
hosts
cat /host/sys/web
cat /host/sys/health
ls /
```

2. 两边应看到不同的 `pwos-xxxx` 和 UID、`peers=1`、相同 leader UID；角色必须是一边
   leader、另一边 follower。两边拓扑最终都应列出两个节点且全局名字一致。
3. 根据 `hosts` 的 owner 记录，在两边分别读取本地和跨主机节点：

```text
cat /mcu1/sys/health
cat /mcu2/sys/health
fault mcu1 status
fault mcu2 status
```

   四次读取都应成功；跨主机一侧的 `remote_io` read 计数增加，`client_errors`、
   `sync_fail` 和 link parse error 不持续增长。
4. 执行一次无破坏性的跨主机写入验证：先确认目标 owner 在另一块 P4，然后执行
   `fault <mcuN> clear`。再次执行 `fault <mcuN> status`，应成功且 remote write 增加。
5. 断开当前 leader 的 LAN 或关闭该 P4。15 秒后原 follower 应成为 leader，失联 peer
   及其节点从拓扑中删除，本地 MCU 的 health 继续正常。恢复旧 leader 后等待 20 秒，
   两边应再次收敛到相同 leader 和完整拓扑。
6. 重启任一 P4 后，其 epoch 应比该板上次启动值加一；两边仍应独立算出同一 leader。

## Alpha 边界

- TCP host RPC 当前没有 TLS、鉴权和重放防护，只能用于可信隔离 LAN。
- 跨主机只转发节点 read/write；远端子目录 `list` 和 `stat` 尚未转发，根目录
  `ls /` 已使用全局拓扑合并。
- `CTRL_HOST_ADVERTISE` 当前不跨 STM32 relay 转发，只有直接连接主机的节点执行
  authority 端口过滤；下游节点维持已有 upstream 控制语义。
- 固定容量为 8 个 peer、12 个全局节点。TCP server 串行处理有界请求，适合 alpha
  控制和诊断流量，不适合大文件或高吞吐数据面。
