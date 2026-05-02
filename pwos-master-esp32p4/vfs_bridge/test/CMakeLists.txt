cmake_minimum_required(VERSION 3.22)
project(cluster_vfs_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(VFS_BRIDGE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MINI9P_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../mini9p)

add_executable(cluster_vfs_test
    test_main.c
    ${VFS_BRIDGE_ROOT}/cluster_config.c
    ${VFS_BRIDGE_ROOT}/cluster_vfs.c
    ${MINI9P_ROOT}/mini9p_client.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

target_include_directories(cluster_vfs_test PRIVATE
    ${VFS_BRIDGE_ROOT}
    ${MINI9P_ROOT}
)
