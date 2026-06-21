cmake_minimum_required(VERSION 3.16)

project(pwos_host_api_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(HOST_API_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(HOST_SESSIONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../host_sessions)
set(HOST_COORDINATOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../host_coordinator)
set(SHARED_MINI9P_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mini9p)
set(SHARED_MESH2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mesh2)
set(SHARED_LINK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/link)

add_executable(pwos_host_api_test
    test_cluster_vfs.c
    ${HOST_API_DIR}/cluster_vfs.c
    ${HOST_SESSIONS_DIR}/session_manager.c
    ${HOST_COORDINATOR_DIR}/host_coordinator.c
    ${SHARED_MINI9P_DIR}/mini9p_protocol.c
    ${SHARED_MINI9P_DIR}/mini9p_client.c
    ${SHARED_MESH2_DIR}/pwos_mesh2_control.c
    ${SHARED_LINK_DIR}/pwos_link_crc.c
    ${SHARED_LINK_DIR}/pwos_link_frame.c
)

target_include_directories(pwos_host_api_test PRIVATE
    ${HOST_API_DIR}
    ${HOST_SESSIONS_DIR}
    ${HOST_COORDINATOR_DIR}
    ${SHARED_MINI9P_DIR}
    ${SHARED_MESH2_DIR}
    ${SHARED_LINK_DIR}
)
