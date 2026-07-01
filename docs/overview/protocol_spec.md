# 9P-Walkers 协议规范

现行协议分为 STM32 有线平面和 ESP32 主机间平面。源码头文件是字段定义的最终依据。

## 1. Link Frame v2

定义：`pwos-shared/link/pwos_link_frame.h`

所有多字节字段使用小端序，总长度为 `19 + payload_len`，payload 最大 512 字节。

```text
offset size field
0      2    magic = 'M' 'H'
2      1    version = 2
3      1    hdr_len = 19
4      1    type
5      1    flags
6      1    src
7      1    dst
8      1    ttl
9      2    seq
11     2    ack
13     2    payload_len
15     2    header_crc
17     2    payload_crc
19     N    payload
```

CRC 使用 CRC-16/CCITT-FALSE。头 CRC 覆盖 `magic..payload_len`，payload CRC 只覆盖
payload。流式 parser 支持任意分块输入、半帧保留、噪声重同步和非法长度拒绝。

### 1.1 类型

| 值 | 名称 | 作用域 |
|---:|---|---|
| `0x01` | `LINK_HELLO` | 物理邻居发现 |
| `0x02` | `LINK_HELLO_ACK` | HELLO 确认 |
| `0x03` | `LINK_HEARTBEAT` | 链路保活预留 |
| `0x04` | `LINK_ERROR` | 链路错误 |
| `0x10` | `CTRL_NODE_REGISTER` | 节点注册 |
| `0x11` | `CTRL_ADDR_ASSIGN` | 地址和 lease 分配 |
| `0x12` | `CTRL_LEASE_RENEW` | lease 续期 |
| `0x13` | `CTRL_LEASE_ACK` | 续期确认 |
| `0x14` | `CTRL_LINK_STATE` | 邻接上报 |
| `0x15` | `CTRL_ROUTE_UPDATE` | 路由设置/删除 |
| `0x16` | `CTRL_HOST_ADVERTISE` | 主机 authority 通告 |
| `0x17` | `CTRL_TIME_SYNC` | 节点 wall-clock 时间同步 |
| `0x1f` | `CTRL_ERROR` | 控制面错误 |
| `0x80` | `DATA_MINI9P` | mini9P |
| `0x81` | `DATA_RPC` | MCU RPC |
| `0x82` | `DATA_JOB` | Job System |
| `0x83` | `DATA_BULK` | 大数据预留 |

`0x00` 是 host 地址，`0xff` 是未分配/广播地址。中继转发时必须递减 TTL。

## 2. Mesh2 控制 payload

定义：`pwos-shared/mesh2/pwos_mesh2_control.h`

控制 payload 版本为 1，使用固定长度和小端序：

| 消息 | 长度 |
|---|---:|
| node register | 24 |
| addr assign | 28 |
| lease renew | 24 |
| lease ack | 28 |
| link state | 24 |
| route update | 12 |
| host advertise | 28 |
| time sync | 32 |

节点身份由 `uid[3] + boot_id` 组成。UID 标识硬件，boot ID 标识一次启动。地址分配和
路由更新由 coordinator 权威生成；节点不能自行选择正式地址。

### 2.1 时间同步

节点每 10 秒向当前 authority 发起一次两报文 NTP 风格交换。`CTRL_TIME_SYNC` payload
固定 32 字节，所有多字节字段为小端序：

```text
offset size field
0      1    version = 1
1      1    kind: 1=request, 2=response
2      1    flags: bit0=server wall-clock valid
3      1    reserved = 0
4      4    sequence
8      8    client_tx_mono_us
16     8    server_rx_unix_us
24     8    server_tx_unix_us
```

节点在收到响应时记录第四时间戳，并按四时间戳公式计算 `mono -> Unix` 偏移及往返延迟。
中继只按路由转发，不修改 payload。authority 改变或 60 秒无成功同步时，节点立即将
wall-clock 标记为无效。

## 3. mini9P

定义：`pwos-shared/mini9p/mini9p_protocol.h`

mini9P 只表达目标节点本地文件语义：attach、walk、open、read、write、stat、clunk。
完整 mini9P 帧作为 `DATA_MINI9P` payload 传输，中继不解析其内容。

主机路径 `/mcu2/sys/health` 在发往节点前转换为本地路径 `/sys/health`。主机通过
全局 wire tag 和 `(DATA_MINI9P, src_addr, tag)` 匹配响应。

STM32 当前命名空间：

```text
/sys/{health,time,tasks,ports,links,neighbors,routes,sessions,queues,log,build,fault}
/compute/{caps,load,jobs}
/display/{status,tile}       # 仅 F429
```

## 4. MCU RPC v1

定义：`pwos-shared/rpc/pwos_rpc_protocol.h`

- 固定头 16 字节，完整内层帧最大 512 字节。
- kind：request、response、cancel、stream chunk、stream end。
- flags：oneway、stream。
- `call_id` 由主机 session manager 分配。
- deadline 是请求预算；超时后主机发送 CANCEL，并释放 pending。
- stream chunk 的 `status` 字段承载从 0 开始的 chunk 序号。

当前内置服务包括 `system.ping/stream/info/notify/delay/fail`。

## 5. Job v1

定义：`pwos-shared/job/pwos_job_protocol.h`

- 固定头 20 字节，完整内层帧最大 512 字节。
- 支持 caps、submit、status、result、cancel 请求/响应。
- 状态：created、queued、assigned、running、done、failed、cancelled、lost。
- kernel：hash、vector add、matmul、Mandelbrot、raytrace tile。
- `request_id` 参与 typed pending；`job_id` 标识远端任务。

节点重启、事务超时或远端任务丢失时，主机将本地 job 标记为 LOST。retry 创建新 job，
不复活旧 boot 上的远端 ID。

## 6. Host RPC v1

定义：

- `pwos-shared/host_rpc/pwos_host_rpc_protocol.h`
- `pwos-shared/host_rpc/pwos_host_rpc_methods.h`

Host RPC 运行在 TCP/9909，不封装进 link frame。每帧由 4 字节网络序 CBOR body 长度
和规范 CBOR map 组成，完整 wire frame 上限 1280 字节。

kind：request、response、cancel、stream chunk、stream end。当前方法覆盖：

- host advertise
- topology owner 查询和同步
- 跨主机节点 read/write
- 分布式推理服务
- `time.exchange` 主机 wall-clock 四时间戳交换

`time.exchange` 的方法 payload 同样为固定 32 字节，但遵循 Host RPC 的网络字节序。
两台主机分别以 SNTP 校准系统时间，并用该方法持续测量 peer 相对偏移和网络延迟；若
本机尚无有效 Unix 时间，可用有效 peer 响应完成首次校时。

当前协议没有 TLS、鉴权和重放保护，只允许部署在可信隔离 LAN。

## 7. 错误与并发规则

1. link CRC 错误只丢弃当前帧，不重置其他端口。
2. 无路由立即返回 `E_NO_ROUTE`，不伪装成 deadline。
3. pending 必须同时校验数据类型、源地址和 tag/request ID。
4. 节点 boot ID 改变后，旧响应、fid、RPC 和 Job 状态全部失效。
5. 控制队列优先于普通数据队列。
6. 所有协议输入都必须先校验版本、固定长度和上限，再访问字段。
7. lease、deadline、重试和任务调度只使用单调时钟；wall-clock 只用于跨机时间戳。
