# pwos-shared

`pwos-shared` 存放主控和从机共同使用的协议代码。当前重构线只保留新链路层、
`mesh2` 控制面和 mini9P；旧 `pwos-shared/mesh/` 已删除，避免继续误用旧轮询
transport / legacy mesh runtime。

## 当前模块

```text
pwos-shared/
├── link/       # M1 链路帧、CRC、流式 parser
├── mesh2/      # M3/M4 控制面 payload 编解码
├── mini9p/     # mini9P 协议本体、client、server
├── csc/        # lttit 通信栈实验/供应商代码，暂未接入当前固件
└── vfs/        # 预留说明
```

## 构建关系

- P4 coordinator 使用 `link/`、`mesh2/`、`mini9p/`。
- STM32 从机使用 `link/`、`mesh2/`、`mini9p/`。
- `csc/` 当前不在默认固件构建路径内，后续若继续复用 lttit 通信栈再单独接入。

## PC 测试

```bash
cmake -S pwos-shared/link/tests -B /tmp/pwos-link-tests
cmake --build /tmp/pwos-link-tests
/tmp/pwos-link-tests/pwos_link_test

cmake -S pwos-shared/mesh2/tests -B /tmp/pwos-mesh2-tests
cmake --build /tmp/pwos-mesh2-tests
/tmp/pwos-mesh2-tests/pwos_mesh2_control_test

gcc -std=c11 -Wall -Wextra \
  -I pwos-shared/mini9p \
  pwos-shared/mini9p/test_mini9p_client_host.c \
  pwos-shared/mini9p/mini9p_client.c \
  pwos-shared/mini9p/mini9p_protocol.c \
  -o /tmp/test_mini9p_client_host
/tmp/test_mini9p_client_host
```
