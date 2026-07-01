# 主机网络与多主机

## 1. 物理网络

- ESP32-P4 通过 IP101 Ethernet 接入路由器。
- ESP32-S3 以 WiFi STA 接入同一网络，默认 SSID/密码均为 `pwos-network`。
- 两台主机分别通过 UART 管理自己的 STM32 子树。
- STM32 link frame 不在 IP 网络中透传。

## 2. 发现和选主

每台主机从 MAC 派生稳定 UID 和 `pwos-xxxx.local` hostname，并发布：

```text
service: _pwos._tcp
port:    9909
```

NVS 保存 host epoch。主机按 `(epoch, priority, host_uid)` 排序选举 leader；
P4 默认 priority 300，S3 默认 priority 200。peer 超时后从成员表移除并重新收敛。

## 3. Host RPC

Host RPC 使用 TCP 长度前缀和 bounded CBOR，提供：

- advertise
- topology owner 查询/同步
- 跨主机节点 read/write
- 分布式推理调用
- `time.exchange` wall-clock 偏移/延迟测量

P4 和 S3 都通过 `pool.ntp.org` 启动 SNTP。Host RPC discovery 每轮完成 advertise 后，
使用四时间戳交换测量 peer 相对本机 wall-clock 的 `offset_us` 和链路 `delay_us`。
若一台主机尚未获得有效 Unix 时间、另一台已经有效，前者会用 peer 的估计时间完成
首次校时；后续仍由 SNTP 负责系统时钟校准。

leader 分配全局 `mcuN` 名称。follower 保存全局名到 owner 本地名的映射；用户始终访问
全局路径，不直接使用 owner 的局部短地址。

## 4. 可观测性

P4 WebShell：

```text
hosts
cat /host/sys/hosts
cat /host/sys/time
cat /host/sys/topology
cat /host/sys/routes
cat /host/sys/web
net status
```

跨主机读取时，目标 owner 的 coordinator 执行真实 mini9P 请求，结果再经 host RPC
返回调用主机。

## 5. Alpha 限制

- 没有 TLS、鉴权和重放保护，只能用于可信隔离 LAN。
- 跨主机主要支持 read/write；远端目录 list/stat 尚未完整代理。
- 固定容量适合控制和诊断流量，不适合大文件或视频流。
- 网络分区时两侧可能短暂各自选主，恢复后依靠 epoch/priority/UID 收敛。
- 当前同步没有硬件时间戳；offset 精度受 TCP/UART 路径不对称和任务调度抖动限制。
- deadline、选举超时和 lease 使用单调时钟，不受 SNTP 或 `settimeofday` 跳变影响。
