cmake_minimum_required(VERSION 3.16)

project(pwos_host_rpc_protocol_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(pwos_host_rpc_protocol_test
    test_host_rpc_protocol.c
    ../pwos_host_rpc_protocol.c
    ../pwos_host_election.c
)

target_include_directories(pwos_host_rpc_protocol_test PRIVATE ..)
target_compile_options(pwos_host_rpc_protocol_test PRIVATE -Wall -Wextra -Werror)

add_test(NAME pwos_host_rpc_protocol_test COMMAND pwos_host_rpc_protocol_test)
