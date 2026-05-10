cmake_minimum_required(VERSION 3.22)
project(cluster_vfs_test C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(VFS_BRIDGE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MINI9P_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mini9p)

add_executable(cluster_vfs_test
    test_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../transport/uart_transport.c
    ${VFS_BRIDGE_ROOT}/cluster_config.c
    ${VFS_BRIDGE_ROOT}/cluster_vfs.c
    ${MINI9P_ROOT}/mini9p_client.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

target_include_directories(cluster_vfs_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../transport
    ${VFS_BRIDGE_ROOT}
    ${MINI9P_ROOT}
)
