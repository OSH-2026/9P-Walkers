cmake_minimum_required(VERSION 3.16)
project(pwos_rpc_protocol_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

add_executable(pwos_rpc_protocol_test
    test_rpc_protocol.c
    ../pwos_rpc_protocol.c)

target_include_directories(pwos_rpc_protocol_test PRIVATE ..)
target_compile_options(pwos_rpc_protocol_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME pwos_rpc_protocol_test COMMAND pwos_rpc_protocol_test)
