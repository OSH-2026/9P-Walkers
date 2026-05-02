# cluster_vfs 测试说明

## 1. 测试目标

`vfs_bridge/test/test_main.c` 用于验证 `cluster_vfs` 与 `mini9p_client` 之间的核心调用链，不依赖真实 UART、WiFi 或从机设备。

当前测试覆盖：

- 路由重复添加检查。
- `/` 虚拟根目录 stat。
- `/mcu1` 与 `/mcu10` 的路径边界匹配。
- 直连路由下的路径转换：`/mcu1/dev/temp -> /dev/temp`。
- `open -> read -> close`。
- `open -> write -> close`。
- `stat` 成功路径会 clunk 临时 fid。
- `stat` 错误路径也会 clunk 临时 fid。
- `close` 会释放本地 fd，并把远端 clunk 错误返回给调用者。

## 2. 构建和运行

在仓库根目录执行：

```bash
cmake -S pwos-master-esp32p4/vfs_bridge/test -B /tmp/cluster_vfs_test_build
cmake --build /tmp/cluster_vfs_test_build
/tmp/cluster_vfs_test_build/cluster_vfs_test
```

期望输出：

```text
cluster_vfs test runner start
cluster_vfs tests passed
```

也可以使用测试目录下的脚本：

```bash
pwos-master-esp32p4/vfs_bridge/test/run.sh
```

## 3. 测试架构

测试程序没有启动真实 mini9P server，而是实现了一个帧级 `mock_transport()`。

调用链如下：

```text
cluster_vfs_open/stat/read/write/close
-> m9p_client_*
-> mock_transport
-> 解码 T* 请求
-> 返回对应 R* 响应或 Rerror
```

这样可以测试真实的 mini9P client 编码、tag 校验、type 校验、CRC 校验和错误解析，而不是只 mock 掉函数返回值。

## 4. mock_ctx 的作用

`mock_ctx` 是测试用上下文，承担两个职责：

1. 记录请求

```text
attach_count
walk_count
open_count
read_count
write_count
stat_count
clunk_count
last_walk_path
last_open_fid
last_open_mode
last_stat_fid
last_clunk_fid
```

测试通过这些字段确认 `cluster_vfs` 是否真的发送了预期请求。

例子：

- 打开 `/mcu1/dev/temp` 后，`last_walk_path` 应该是 `/dev/temp`。
- close 后，`last_clunk_fid` 应该等于 open 时使用的远端 fid。
- stat 后，`last_clunk_fid` 应该等于 stat 临时 fid。

2. 控制错误

```text
stat_error_code
clunk_error_code
```

测试可以把它们设置成 Mini9P 错误码，让 mock transport 返回 `Rerror`，从而验证错误路径。

## 5. mock_transport 的行为

`mock_transport()` 会先调用：

```c
m9p_decode_frame(tx, tx_len, &frame)
```

解码客户端发来的请求帧，然后按请求类型返回响应。

对应关系：

```text
TATTACH -> RATTACH
TWALK   -> RWALK
TOPEN   -> ROPEN
TREAD   -> RREAD
TWRITE  -> RWRITE
TSTAT   -> RSTAT 或 RERROR
TCLUNK  -> RCLUNK 或 RERROR
其他    -> RERROR(ENOTSUP)
```

响应帧统一通过：

```c
m9p_encode_frame(...)
```

生成，因此客户端仍会走真实的 mini9P 响应解析逻辑。

## 6. 各测试用例说明

### test_duplicate_route

验证重复添加相同 target：

```text
第一次 cluster_vfs_add_direct("mcu1") -> 0
第二次 cluster_vfs_add_direct("mcu1") -> -M9P_ERR_EBUSY
```

目的是避免路由表中出现多个相同 target，导致后续条目变成不可达死路由。

### test_root_stat

验证：

```text
cluster_vfs_stat("/")
```

应由本地合成虚拟根目录 stat，不访问远端节点。

检查点：

- 返回值为 0。
- `stat.name == "/"`。
- `stat.flags` 带有 `M9P_STAT_DIR`。

### test_path_boundary

验证：

```text
/mcu10/dev/temp
```

不会误命中 `target=mcu1`。

检查点：

- 返回 `-M9P_ERR_ENOENT`。
- `walk_count == 0`，说明没有发出远端 walk。

### test_open_read_close

验证只读主流程：

```text
cluster_vfs_open("/mcu1/dev/temp", M9P_OREAD)
cluster_vfs_read(fd, ...)
cluster_vfs_close(fd)
```

检查点：

- 远端 walk 路径是 `/dev/temp`。
- open 使用的 fid 等于 walk 分配的新 fid。
- read 返回 mock 数据 `data`。
- close 会 clunk open 得到的远端 fid。
- 第二次 close 同一个 fd 返回 `-M9P_ERR_EFID`。

### test_write_ordwr

验证 `M9P_ORDWR` 模式可以写。

检查点：

- write 返回成功。
- `out_written` 等于写入长度。
- write 使用的是 open 得到的同一个远端 fid。

这个测试也覆盖了 `M9P_OREAD == 0` 不能用简单位与判断权限的问题：读写权限应按 access mode 的低两位判断。

### test_stat_success_clunks

验证 stat 成功路径：

```text
walk -> stat -> clunk
```

检查点：

- stat 返回成功。
- 远端 walk 路径是 `/dev/temp`。
- stat name 是 mock 返回的 `temp`。
- clunk 次数为 1。
- clunk 的 fid 等于 stat 使用的临时 fid。

### test_stat_error_clunks

验证 stat 错误路径仍释放临时 fid。

测试设置：

```c
ctx.stat_error_code = M9P_ERR_EIO;
```

此时 mock transport 对 `TSTAT` 返回 `Rerror(EIO)`。

检查点：

- `cluster_vfs_stat()` 返回 `-M9P_ERR_EIO`。
- clunk 次数为 1。
- clunk 的 fid 等于 stat 使用的临时 fid。

### test_close_returns_clunk_error

验证 close 对远端 clunk 错误的处理。

测试设置：

```c
ctx.clunk_error_code = M9P_ERR_EIO;
```

检查点：

- `cluster_vfs_close(fd)` 返回 `-M9P_ERR_EIO`。
- 本地 fd 已经释放。
- 再次 close 同一个 fd 返回 `-M9P_ERR_EFID`。

## 7. 当前测试边界

当前测试已经覆盖核心单路由调用链和一组上层便捷接口：

- `cluster_vfs_read_path()`：路径级读取普通文件，并自动 close。
- `cluster_vfs_write_path()`：路径级写入普通文件，并自动 close。
- `cluster_vfs_list("/")`：本地合成根目录挂载点。
- `cluster_vfs_list("/mcu1/dev")`：读取并解析远端目录项。
- `cluster_vfs_read_path()` 读目录返回 `-M9P_ERR_EISDIR`。
- `cluster_vfs_list()` 列普通文件返回 `-M9P_ERR_ENOTDIR`。
- `cluster_vfs_get_route_state()` 查询 READY/ATTACHED/READY 状态变化。
- `cluster_vfs_remove_route()` 在 fd 未关闭时返回 `-M9P_ERR_EBUSY`。
- `cluster_vfs_detach()` 在 fd 未关闭时返回 `-M9P_ERR_EBUSY`。

仍有一些未覆盖或尚未实现的方向：

- `cluster_vfs_add_route()` 尚未在 `cluster_vfs.c` 实现，因此没有中继路由测试。
- `cluster_vfs_list_routes()` 尚未实现，因此没有批量路由枚举测试。
- 没有覆盖真实 UART/WiFi transport。
- 没有覆盖并发访问、自动重连、route offline 状态。
- 没有覆盖路径规范化，例如 `//`、`.`、`..`。

## 8. 新增测试建议

后续补功能时建议增加：

- `cluster_vfs_add_route("mcu3", "mcu1", client)` 的全局路径转发测试。
- `/mcu1` 和 `/mcu1/` 映射到远端 `/` 的测试。
- open 表满时返回 `-M9P_ERR_EBUSY` 的测试。
- detach 后 open/stat 返回无路由或不可用错误的测试。
- `cluster_vfs_list_routes()` 批量导出 `target/next_hop/state` 的测试。
