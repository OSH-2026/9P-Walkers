# TODO

## P0 — 断言基础设施

- [ ] 增加 `expect_eq(actual, expected)` / `expect_str_eq` / `expect_null`，失败时打印实际值
- [ ] 断言失败即中止当前用例（`expect_*` 返回 bool，调用方检查后 `return`），避免在未初始化/失败状态下继续操作导致级联崩溃
- [ ] 每个用例独立 pass/fail 计数，`main()` 汇总输出"X/Y passed"

## P1 — 协议覆盖

- [ ] Tstat：`m9p_build_tstat` → `m9p_parse_tstat` → `m9p_build_rstat` → `m9p_parse_rstat`
- [ ] Twrite：`m9p_build_twrite` → `m9p_parse_twrite` → `m9p_build_rwrite` → `m9p_parse_rwrite`

## P1 — 错误路径

### Attach
- [ ] 重复 attach（第二次应返回错误）
- [ ] 未 attach 前发送 walk/open/read/stat/clunk（应返回错误）
- [ ] msize=0 协商
- [ ] msize 超大值协商

### Walk
- [ ] 空路径 `""`
- [ ] 多级 walk（先 `/sys`，再 `/sys/health`）
- [ ] 超长路径（`> M9P_MAX_PATH_LEN`）
- [ ] 中间路径不存在（`/sys/nonexist/health`）

### Open
- [ ] open 目录的行为
- [ ] 对只读文件传 `M9P_OWRITE`（应返回 EPERM）
- [ ] 重复 open 同一 fid
- [ ] `M9P_OTRUNC` 模式

### Read
- [ ] offset 超出文件大小（应返回 count=0）
- [ ] offset + count 跨文件末尾边界
- [ ] count=0 零长读取
- [ ] 未 open 就直接 read（应返回错误）

### Clunk
- [ ] clunk 不存在的 fid（应返回 EFID）
- [ ] double clunk 同一 fid
- [ ] clunk 后再用该 fid 做操作

## P2 — 协议层容错

- [ ] 坏 magic 字节（非 `0x39 0x50`）
- [ ] CRC 校验失败
- [ ] 截断帧（payload 不完整）
- [ ] 超过 `negotiated_msize` 的帧
- [ ] 未知消息类型
- [ ] 响应缓冲区过小，无法容纳完整响应帧

## P2 — 假数据节点增强

- [ ] 添加可写节点（用于测试 Twrite）
- [ ] 添加长内容文件（用于测试分片读取、offset 边界）
- [ ] `fake_open` 校验 mode（只读节点拒绝 OWRITE）
- [ ] `fake_read` 校验文件已 open 状态
- [ ] `fake_clunk` 校验 `was_open`

## P3 — 服务端 API

- [ ] `m9p_server_get_default_config` 直接测试，验证每个字段的默认值
- [ ] `m9p_server_reset_session` + 重新 attach 完整重连流程测试

## P3 — 结构性改进

- [ ] 表驱动/参数化测试，减少"共性逻辑 × 多种消息类型"的重复代码
- [ ] 测试发现自动化（如函数指针数组 `g_tests[]`），新增测试无需修改 `main()`
