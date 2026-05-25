cmake_minimum_required(VERSION 3.16)

project(pc_master_emulator C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(MINI9P_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mini9p)
set(MESH_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh)
set(VFS_BRIDGE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../pwos-master-esp32p4/vfs_bridge)

add_executable(pc_master_emulator
    main.c
    mesh_uart_transport_stub.c
    ${VFS_BRIDGE_ROOT}/cluster_config.c
    ${VFS_BRIDGE_ROOT}/cluster_vfs.c
    ${VFS_BRIDGE_ROOT}/mesh_host_runtime.c
    ${MESH_ROOT}/cluster/cluster.c
    ${MESH_ROOT}/envelope/mesh_protocal.c
    ${MESH_ROOT}/processer/mesh_processer.c
    ${MINI9P_ROOT}/mini9p_client.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

target_include_directories(pc_master_emulator PRIVATE
    ${VFS_BRIDGE_ROOT}
    ${MINI9P_ROOT}
    ${MESH_ROOT}/cluster
    ${MESH_ROOT}/envelope
    ${MESH_ROOT}/processer
    ${MESH_ROOT}/transport
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pc_master_emulator PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()
