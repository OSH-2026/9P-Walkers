cmake_minimum_required(VERSION 3.16)

project(pwos_host_rpc_endpoint_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(HOST_RPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(HOST_SESSIONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../host_sessions)
set(SHARED_HOST_RPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/host_rpc)
set(SHARED_MINI9P_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mini9p)

add_executable(pwos_host_rpc_endpoint_test
    test_host_rpc_endpoint.c
    ${HOST_RPC_DIR}/host_rpc_service.c
    ${HOST_RPC_DIR}/host_rpc_peer_client.c
    ${SHARED_HOST_RPC_DIR}/pwos_host_rpc_protocol.c
)

target_include_directories(pwos_host_rpc_endpoint_test PRIVATE
    ${HOST_RPC_DIR}
    ${HOST_SESSIONS_DIR}
    ${SHARED_HOST_RPC_DIR}
    ${SHARED_MINI9P_DIR}
)
target_compile_options(pwos_host_rpc_endpoint_test PRIVATE -Wall -Wextra -Werror)

add_test(NAME pwos_host_rpc_endpoint_test COMMAND pwos_host_rpc_endpoint_test)
