# PWOS CSC integration

本目录用于把 `lttit` 的 CSC 通信栈按协议层切片复用到 9P-Walkers。

来源：

- upstream path: `/home/hb/lkvo/projects/lttit`
- upstream license: MIT，见 `vendor/lttit/LICENSE`

当前接入状态：

- `ccnet`：已 vendor，已加 PC smoke test，验证 Dijkstra next-hop 和本地投递。
- `scp`：已 vendor，已加 PC 编译/生命周期 smoke test；尚未做握手和重传链路测试。
- `ccrpc`：已 vendor，已加 PC 编译/初始化 smoke test；尚未映射到 mini9P/cluster_vfs。

边界约定：

- 物理帧层仍使用 `pwos-shared/link/` 的 `pwos_link_frame` 和增量 parser。
- CSC 从完整 payload 开始工作，不直接接触 UART/DMA/ISR。
- STM32 热路径不得使用 PC 版 `heap_malloc` 适配；M2/M3 接硬件前必须换成静态池或受控 allocator。

下一步：

1. 写 `pwos_csc_link_adapter`：把 `ccnet_output()` 的包封装进 `pwos_link_frame`
   payload，把 `pwos_link_parser` 输出的 payload 送入 `ccnet_input()`。
2. 写两节点 PC loopback 测试：`ccrpc -> scp -> ccnet -> pwos_link_frame -> ccnet -> scp -> ccrpc`。
3. 只有 PC loopback 稳定后，再接 STM32 FreeRTOS 队列和 UART DMA。
