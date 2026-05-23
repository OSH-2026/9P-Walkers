# Mesh 模块测试结果报告

## 1. 测试目标

本报告用于记录 mesh 模块主机侧测试执行结果，重点验证：

1. processor 与串口回调接口（send/receive）联动。
2. processor 与 cluster 路由/控制回调联动。
3. processor 与 mini9P server/client 回调联动。
4. mesh 协议全部消息类型与辅助 API 的编解码正确性。
5. processor 关键异常路径的错误行为与容错行为。
6. 压力与鲁棒性场景下的稳定性（高频连续帧、混合队列、重复帧、边界 payload）。

## 2. 测试环境

- 日期：2026-05-23
- 操作系统：Windows
- 编译器：gcc 8.1.0（x86_64-posix-seh-rev0，MinGW-W64）
- 测试目录：pwos-shared/mesh/test

## 3. 测试用例摘要（按测试程序）

### 3.1 模块联调与异常路径（test_mesh_module_host）

- test_forward_non_local_frame
- test_local_ping_generates_pong_reply
- test_local_mini9p_request_to_server
- test_poll_once_dispatches_local_mini9p_response
- test_bad_crc_frame_rejected
- test_hop_exhausted_returns_no_route
- test_no_route_returns_no_route
- test_control_handler_null_is_tolerated
- test_invalid_local_mini9p_payload_returns_bad_frame
- test_high_frequency_forward_burst
- test_duplicate_response_frames_processed_twice
- test_poll_once_mixed_queue_sequence

### 3.2 协议全覆盖（test_mesh_protocol_all_host）

- test_crc_known_vector
- test_encode_decode_and_frame_guards
- test_payload_boundaries
- test_prepare_forward
- test_crc_detects_single_byte_corruption
- test_control_type_and_error_names
- test_mini9p_build_and_parse
- test_register_roundtrip
- test_register_header_semantics
- test_assign_roundtrip_and_invalid_name_len
- test_assign_empty_name_roundtrip
- test_ping_and_pong_roundtrip
- test_time_sync_roundtrip
- test_route_update_roundtrip_and_invalid_action
- test_link_state_roundtrip
- test_error_roundtrip

## 4. 执行命令

- 模块联调与异常路径：

```powershell
gcc -o test_mesh_module_host.exe test_mesh_module_host.c ..\envelope\mesh_protocal.c ..\processer\mesh_processer.c ..\cluster\cluster.c ..\..\mini9p\mini9p_protocol.c -I.. -I..\envelope -I..\processer -I..\cluster -I..\..\mini9p
.\test_mesh_module_host.exe
```

- 协议全覆盖：

```powershell
gcc -o test_mesh_protocol_all_host.exe test_mesh_protocol_all_host.c ..\envelope\mesh_protocal.c ..\..\mini9p\mini9p_protocol.c -I.. -I..\envelope -I..\..\mini9p
.\test_mesh_protocol_all_host.exe
```

## 5. 执行结果

### 5.1 test_mesh_module_host

- 编译结果：成功
- 运行输出：mesh module host tests passed (normal + abnormal + stress)
- 进程退出码：0
- 用例结果：12/12 通过

分项说明：

1. test_forward_non_local_frame：验证非本机目的帧转发与 hop 递减。
2. test_local_ping_generates_pong_reply：验证控制面 PING 到 PONG 自动回包。
3. test_local_mini9p_request_to_server：验证本机 mini9P T* 分流到 server 并封装 R* 回发。
4. test_poll_once_dispatches_local_mini9p_response：验证 poll_once + mini9P R* 分流到 client。
5. test_bad_crc_frame_rejected：验证坏 CRC 帧被拒收并返回 BAD_FRAME。
6. test_hop_exhausted_returns_no_route：验证 hop 耗尽时不转发并返回 NO_ROUTE。
7. test_no_route_returns_no_route：验证无路由时返回 NO_ROUTE。
8. test_control_handler_null_is_tolerated：验证 control_handler 为空时本机控制帧可容错吞掉。
9. test_invalid_local_mini9p_payload_returns_bad_frame：验证本机收到非法 mini9P 负载时返回 BAD_FRAME。
10. test_high_frequency_forward_burst：验证 16 帧连续转发下 next_hop 与 hop 递减稳定。
11. test_duplicate_response_frames_processed_twice：验证重复 R* 帧会被稳定处理两次（当前无去重语义）。
12. test_poll_once_mixed_queue_sequence：验证混合队列（R*/T*/转发/坏帧）顺序处理的稳定性。

### 5.2 test_mesh_protocol_all_host

- 编译结果：成功
- 运行输出：mesh protocol all tests passed
- 进程退出码：0
- 用例结果：17/17 通过

覆盖说明：

1. 覆盖全部类型 build/parse：MINI9P、REGISTER、ASSIGN、PING、PONG、TIME_SYNC、ROUTE_UPDATE、LINK_STATE、ERROR。
2. 覆盖协议辅助 API：CRC、控制类型判定、forward hop 计算、错误码字符串映射。
3. 覆盖关键反向校验：Magic 错误、长度不一致、CRC 错误、版本错误、无效 action、无效 name_len。
4. 覆盖边界行为：最大 payload、空 payload、ASSIGN 空节点名、REGISTER 头字段语义。
5. 覆盖抗破坏行为：逐字节篡改时 CRC 检测失效帧。

### 5.3 一次修复记录

- 首次执行协议全覆盖测试时，ASSIGN 用例中的测试节点名长度写成 33，触发断言失败。
- 已修正为严格 31 字符后复测通过。

## 6. 结论

- 当前 mesh 模块在“串口回调接口 + cluster 控制/路由接口 + mini9P 分流接口”三条主链路上已形成可运行闭环。
- 已新增并通过 processor 异常路径、压力路径与 mesh 协议全覆盖测试，能支持后续回归。
- 下一阶段可继续增加随机化长时测试（例如 10^4 帧级别）与多节点拓扑切换场景。
