cmake_minimum_required(VERSION 3.16)

project(pwos_host_coordinator_tests C)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(HOST_COORDINATOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SHARED_MESH2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mesh2)
set(SHARED_LINK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/link)

add_executable(pwos_host_coordinator_test
    test_host_coordinator.c
    ${HOST_COORDINATOR_DIR}/host_coordinator.c
    ${SHARED_MESH2_DIR}/pwos_mesh2_control.c
    ${SHARED_LINK_DIR}/pwos_link_crc.c
    ${SHARED_LINK_DIR}/pwos_link_frame.c
)

target_include_directories(pwos_host_coordinator_test PRIVATE
    ${HOST_COORDINATOR_DIR}
    ${SHARED_MESH2_DIR}
    ${SHARED_LINK_DIR}
)

add_test(NAME pwos_host_coordinator_test COMMAND pwos_host_coordinator_test)
