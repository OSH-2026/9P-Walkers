# mini9p server testbench

这个目录用于在 PC 上验证 `mini9p_server` 的协议分发和 fid 状态机，不依赖 UART、HAL、RTOS 或 littlefs。

测试链路：

```text
test_main
  -> m9p_build_t*
  -> m9p_server_handle_frame
  -> fake backend
  -> m9p_parse_r*
```

当前覆盖：

```text
Tattach -> Rattach
Twalk /sys/health -> Rwalk
Topen -> Ropen
Tread -> Rread
Tclunk -> Rclunk
非法路径 -> Rerror ENOENT
非法 fid -> Rerror EFID
```

运行方式：

```bash
cd pwos-slave/User/mini9p/test
cmake -S . -B build
cmake --build build
./build/mini9p_server_test
```
