# PWOS 日志使用说明

`pwos_log` 是一个全局的固定大小环形日志,用于节点诊断。任何 slave 侧的
模块都可以包含 `pwos_log.h` 并追加记录,无需自行持有日志缓冲区或
传递回调。

## 基本用法

在 C 文件中包含头文件:

```c
#include "pwos_log.h"
```

记录一个通用事件:

```c
pwos_log_event(10u, value_a, value_b, value_c, rc);
```

字段是有意设计为通用的:

- `event`:调用方自定义的事件 id。`0` 会被规整为 `PWOS_LOG_EVENT_GENERIC`。
- `a`、`b`、`c`:调用方自定义的 32 位值。
- `rc`:结果码,成功时通常为 `0`,失败时为负的 `MESH_ERR_*` /
  `M9P_ERR_*` 风格的值。

模块内部会存储时间戳。在 STM32 构建上使用 `HAL_GetTick()`;
非 HAL 构建回退为 `0`。

## Mesh 发送日志

Mesh 发送路径应使用结构化辅助函数,以便 `/sys/log` 保留现有的
mesh 调试文本格式:

```c
pwos_log_mesh_tx(
    PWOS_LOG_EVENT_MESH_SEND,
    type,
    src,
    dst,
    next_hop,
    port_id,
    len,
    mini9p_type,
    rc);
```

直接通过物理端口发送时使用 `PWOS_LOG_EVENT_MESH_SEND_PORT`。
如果 Mesh 帧不承载 Mini9P,或 Mini9P payload 无法解析,`mini9p_type`
传 `0xff`。

## 读取日志

日志通过节点 VFS 暴露在:

```text
/sys/log
```

从 PC master 命名空间读取:

```text
/mcuN/sys/log
```

PC master 模拟器在正常冒烟测试完成后会读取该路径,
在 runtime 错误退出前也会尝试读取它。

## 输出格式

通用事件的格式为:

```text
t=<ms> ev=event id=<event> a=<a> b=<b> c=<c> rc=<rc>
```

Mesh TX 事件的格式为:

```text
t=<ms> ev=send type=0x.. src=0x.. dst=0x.. next=0x.. port=<port> len=<len> rc=<rc>
```

如果 Mesh 帧承载并成功解析出 Mini9P 帧,会额外包含:

```text
m9p=0x..
```

直接端口发送使用 `ev=send_port`。

## 限制

- 环形缓冲区当前保留最近 64 条记录。
- 缓冲区满时按时间顺序覆盖最旧的记录。
- 没有动态内存分配。
- 没有过滤、级别控制、清除命令,也不支持多个日志区。
- `pwos_log_init()` 会清空全局环形缓冲区,在节点 Mini9P
  初始化过程中被调用。
