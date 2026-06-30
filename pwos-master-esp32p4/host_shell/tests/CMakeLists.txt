cmake_minimum_required(VERSION 3.16)

project(pwos_command_service_tests C)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(HOST_SHELL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(HOST_SESSIONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../host_sessions)
set(SHARED_MINI9P_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mini9p)
set(SHARED_RPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/rpc)

add_executable(pwos_command_service_test
    test_command_service.c
    ${HOST_SHELL_DIR}/command_service.c
    ${HOST_SESSIONS_DIR}/session_manager.c
    ${SHARED_MINI9P_DIR}/mini9p_protocol.c
    ${SHARED_MINI9P_DIR}/mini9p_client.c
    ${SHARED_RPC_DIR}/pwos_rpc_protocol.c
)

target_include_directories(pwos_command_service_test PRIVATE
    ${HOST_SHELL_DIR}
    ${HOST_SESSIONS_DIR}
    ${SHARED_MINI9P_DIR}
    ${SHARED_RPC_DIR}
)

target_compile_options(pwos_command_service_test PRIVATE -Wall -Wextra -Werror)

add_test(NAME pwos_command_service_test COMMAND pwos_command_service_test)
