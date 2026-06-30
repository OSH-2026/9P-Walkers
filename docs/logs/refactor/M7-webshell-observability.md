# M7 WebShell / LAN / 可观测性

日期：2026-06-29  
基线：`f8c368f` 之后的工作树  
状态：完成（2026-06-30 用户确认上板无问题）

## 实现范围

- ESP32-P4 Function EV Board IP101 有线 LAN：PHY addr 1，reset GPIO 51，DHCP，
  mDNS `pwos.local`。网络初始化失败不会停止 coordinator。
- HTTP `/`、`/health`、WebSocket `/ws`；前端嵌入固件，不依赖 CDN。
- WebSocket 回调只接收并投递固定队列，命令由独立 worker 执行。
- 每条命令记录 `(socket_fd, generation)`；断线或 fd 复用后的旧结果直接丢弃。
- deadline、命令失败、队列满只返回当前客户端，不关闭 WebSocket。
- 命令统一通过 `cluster_vfs`，不增加 UART RX consumer。
- 主机 `/host/sys/{health,links,topology,routes,sessions,web,log}`。
- 从机 `/sys/{health,tasks,ports,links,neighbors,routes,sessions,queues,log,build}`。
- Debug 从机 `/sys/fault` 及 `fault <mcuN> ...` 命令。
- 删除未进入重构后构建的旧 `node_vfs/sys_vfs/dev_vfs/lfs_vfs`。

## 故障注入命令

```text
fault mcu2 status
fault mcu2 drop port 0 20
fault mcu2 delay port 0 100
fault mcu2 corrupt port 0 10
fault mcu1 down port 0
fault mcu1 recover port 0
fault mcu2 reboot-self
fault mcu2 clear
```

注入只在 F407 Debug 构建启用。`down` 同时丢弃该端口 TX/RX；所有命中数从
`/sys/fault` 查询。远程执行 `down` 时必须选择 relay 的下游端口，保留其上游
控制路径供 `recover` 使用。delay 在共享 `link_tx_task` 执行，只用于短时恢复测试。

## 已执行验证

- ESP32-P4 IDF 6.0 完整构建通过，固件 `0x87d50`，app 分区剩余 47%。
- F407 Debug 构建通过：RAM 66472 B / 128 KiB（50.71%），FLASH 81692 B。
- command service、host session、host API、local VFS PC 测试通过。
- local VFS 新增 64 字节分页测试，确认目录读取不拆分 dirent。
- `git diff --check` 通过。

## 上板验收

1. 烧录 P4 与两块 F407，确认日志出现 LAN link up、DHCP 地址及两个节点 health。
2. 浏览器访问 `http://pwos.local/`，连续执行 `host`、`net status`、`ls /`。
3. 两个浏览器分别连续读取 mcu1/mcu2，确认输出不串。
4. 断开 mcu2，执行 `cat /mcu2/sys/health` 得到 deadline；随后读取 mcu1 成功且
   WebSocket 未断开。
5. 刷新一个浏览器，确认旧命令结果不会投递到新连接。
6. 拔插网线，确认 mesh coordinator 日志持续、节点不重新分配地址。
7. 依次执行 drop/corrupt/down/recover，检查 `/sys/fault`、`/sys/ports` 和
   `/host/sys/sessions` 计数，并确认清除注入后控制面收敛。
8. 两客户端保持 30 分钟 idle 后再次执行命令。

2026-06-30：用户确认以上 M7 行为无问题，进入 M8。
