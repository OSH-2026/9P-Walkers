# Link Frame v2 线缆规范

旧文档中的 “mesh envelope v1” 已被 `pwos-shared/link` 的 link frame v2 替代。
现行字段、类型和值以 `pwos-shared/link/pwos_link_frame.h` 为准。

## 固定头

```text
magic(2) version(1) hdr_len(1) type(1) flags(1)
src(1) dst(1) ttl(1) seq(2) ack(2) payload_len(2)
header_crc(2) payload_crc(2)
```

- 头长度：19 字节。
- 最大 payload：512 字节。
- 最大 wire frame：531 字节。
- 多字节字段：little-endian。
- magic：ASCII `MH`。
- version：2。
- CRC：CRC-16/CCITT-FALSE。

## 校验顺序

1. 查找 `MH`。
2. 校验 version 和 `hdr_len == 19`。
3. 校验 `payload_len <= 512`。
4. 校验 header CRC。
5. 收齐 payload 后校验 payload CRC。
6. 输出只读 frame view，或丢弃候选并继续同步。

parser 必须支持 DMA 分块边界落在任意字段中，不能假设一次回调包含完整帧。

## 转发

- `dst == local_addr`：投递本地控制面或数据面。
- `dst != local_addr`：查本地 route，TTL 递减后发往下一跳。
- `ttl == 0`、无 route、目标端口 down：丢弃并增加相应统计。
- LINK 类型仅在物理邻居之间处理，不跨 relay 转发。

完整类型和上层 payload 见 [protocol_spec.md](protocol_spec.md)。
