cmake_minimum_required(VERSION 3.16)

project(pwos_host_session_tests C)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

set(HOST_SESSIONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(HOST_RPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../host_rpc)
set(SHARED_MINI9P_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mini9p)
set(SHARED_RPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/rpc)

add_executable(pwos_host_session_test
    test_session_manager.c
    ${HOST_SESSIONS_DIR}/session_manager.c
    ${HOST_RPC_DIR}/rpc_client.c
    ${SHARED_MINI9P_DIR}/mini9p_protocol.c
    ${SHARED_MINI9P_DIR}/mini9p_client.c
    ${SHARED_RPC_DIR}/pwos_rpc_protocol.c
)

target_include_directories(pwos_host_session_test PRIVATE
    ${HOST_SESSIONS_DIR}
    ${HOST_RPC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/link
    ${SHARED_MINI9P_DIR}
    ${SHARED_RPC_DIR}
)

target_compile_options(pwos_host_session_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_link_libraries(pwos_host_session_test PRIVATE Threads::Threads)

add_test(NAME pwos_host_session_test COMMAND pwos_host_session_test)
