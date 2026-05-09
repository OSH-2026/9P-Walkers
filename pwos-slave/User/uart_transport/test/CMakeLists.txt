cmake_minimum_required(VERSION 3.16)

project(mini9p_server_test C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(mini9p_server_test
    test_main.c
    ../mini9p_protocol.c
    ../mini9p_server.c
)

target_include_directories(mini9p_server_test PRIVATE
    ..
)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mini9p_server_test PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endif()
