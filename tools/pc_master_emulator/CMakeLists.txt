cmake_minimum_required(VERSION 3.16)

project(pc_master_emulator C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# 路径配置：指向仓库内的共享模块
set(MINI9P_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mini9p)
set(MESH_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh)
set(MASTER_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../pwos-master-esp32p4)
set(MASTER_CLUSTER_ROOT ${MASTER_ROOT}/cluster)
set(MASTER_MESH_ROOT ${MASTER_ROOT}/mesh)
set(VFS_BRIDGE_ROOT ${MASTER_ROOT}/vfs_bridge)

# 源文件说明：
#   main.c                        — 程序入口，串口 + mesh runtime + cluster_vfs smoke test
#   mesh_uart_transport_stub.c     — mesh_uart_transport 空桩（链接用，真实传输在 main.c）
#
# 以下为链接进来的仓库模块：
#   VFS bridge:
#     cluster_config.c             — 集群配置初始化（mesh_host 用）
#     cluster_host_vfs.c                — 集群虚拟文件系统（attach / read_path 等）
#     mesh_host_runtime.c          — 主机侧 mesh runtime（节点管理 + transport 回调）
#
#   Mesh 子系统:
#     cluster.c                    — 集群成员与链路管理
#     mesh_protocal.c              — mesh 帧封装/解封装
#     mesh_processer.c             — mesh 帧处理器
#
#   Mini9P 子系统:
#     mini9p_client.c              — Mini9P 客户端（Twalk / Topen / Tread / Tclunk）
#     mini9p_protocol.c             — Mini9P 帧编解码（协议层）
add_executable(pc_master_emulator
    main.c
    mesh_uart_transport_stub.c
    ${MASTER_CLUSTER_ROOT}/cluster_config.c
    ${VFS_BRIDGE_ROOT}/cluster_host_vfs.c
    ${MASTER_MESH_ROOT}/mesh_host_runtime.c
    ${MASTER_MESH_ROOT}/mesh_host_service.c
    ${MESH_ROOT}/cluster/cluster.c
    ${MESH_ROOT}/envelope/mesh_protocal.c
    ${MESH_ROOT}/processer/mesh_processer.c
    ${MINI9P_ROOT}/mini9p_client.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

# 头文件搜索路径
target_include_directories(pc_master_emulator PRIVATE
    ${MASTER_CLUSTER_ROOT}
    ${MASTER_MESH_ROOT}
    ${VFS_BRIDGE_ROOT}
    ${MINI9P_ROOT}
    ${MESH_ROOT}/cluster
    ${MESH_ROOT}/envelope
    ${MESH_ROOT}/processer
    ${MESH_ROOT}/transport
)

# 编译警告级别
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pc_master_emulator PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()