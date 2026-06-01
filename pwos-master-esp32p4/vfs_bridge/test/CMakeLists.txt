cmake_minimum_required(VERSION 3.22)
project(cluster_vfs_test C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(VFS_BRIDGE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MASTER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MASTER_CLUSTER_ROOT ${MASTER_ROOT}/cluster)
set(MINI9P_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mini9p)
set(MESH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mesh)

add_executable(cluster_vfs_test
    test_main.c
    ${MASTER_CLUSTER_ROOT}/cluster_config.c
    ${VFS_BRIDGE_ROOT}/cluster_host_vfs.c
    ${MESH_ROOT}/cluster/cluster.c
    ${MESH_ROOT}/envelope/mesh_protocal.c
    ${MINI9P_ROOT}/mini9p_client.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

add_executable(mesh_host_service_test
    test_mesh_host_service.c
    ${MASTER_ROOT}/mesh/mesh_host_service.c
    ${MASTER_ROOT}/mesh/mesh_host_runtime.c
    ${MASTER_CLUSTER_ROOT}/cluster_config.c
    ${VFS_BRIDGE_ROOT}/cluster_host_vfs.c
    ${MESH_ROOT}/cluster/cluster.c
    ${MESH_ROOT}/processer/mesh_processer.c
    ${MESH_ROOT}/envelope/mesh_protocal.c
    ${MINI9P_ROOT}/mini9p_client.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

add_executable(mesh_host_runtime_test
    test_mesh_host_runtime.c
    ${MASTER_ROOT}/mesh/mesh_host_runtime.c
    ${MASTER_ROOT}/mesh/mesh_host_service.c
    ${MASTER_CLUSTER_ROOT}/cluster_config.c
    ${VFS_BRIDGE_ROOT}/cluster_host_vfs.c
    ${MESH_ROOT}/cluster/cluster.c
    ${MESH_ROOT}/processer/mesh_processer.c
    ${MESH_ROOT}/transport/mesh_uart_transport.c
    ${MESH_ROOT}/envelope/mesh_protocal.c
    ${MINI9P_ROOT}/mini9p_client.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

target_include_directories(mesh_host_service_test PRIVATE
    ${MASTER_ROOT}/mesh
    ${MASTER_CLUSTER_ROOT}
    ${VFS_BRIDGE_ROOT}
    ${MINI9P_ROOT}
    ${MESH_ROOT}/transport
    ${MESH_ROOT}/cluster
    ${MESH_ROOT}/processer
    ${MESH_ROOT}/envelope
)

target_include_directories(mesh_host_runtime_test PRIVATE
    ${MASTER_ROOT}/mesh
    ${MASTER_CLUSTER_ROOT}
    ${VFS_BRIDGE_ROOT}
    ${MINI9P_ROOT}
    ${MESH_ROOT}/transport
    ${MESH_ROOT}/cluster
    ${MESH_ROOT}/processer
    ${MESH_ROOT}/envelope
)

target_include_directories(cluster_vfs_test PRIVATE
    ${MASTER_CLUSTER_ROOT}
    ${VFS_BRIDGE_ROOT}
    ${MINI9P_ROOT}
    ${MESH_ROOT}/cluster
    ${MESH_ROOT}/envelope
)
