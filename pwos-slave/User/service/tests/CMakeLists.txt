cmake_minimum_required(VERSION 3.16)

project(pwos_rpc_service_tests C)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SERVICE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SHARED_RPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../pwos-shared/rpc)

add_executable(pwos_rpc_service_test
    test_rpc_service.c
    ${SERVICE_DIR}/rpc_service.c
    ${SHARED_RPC_DIR}/pwos_rpc_protocol.c
)

target_include_directories(pwos_rpc_service_test PRIVATE
    ${SERVICE_DIR}
    ${SHARED_RPC_DIR}
)

target_compile_options(pwos_rpc_service_test PRIVATE -Wall -Wextra -Werror)

add_test(NAME pwos_rpc_service_test COMMAND pwos_rpc_service_test)
