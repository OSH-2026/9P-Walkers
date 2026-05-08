cmake_minimum_required(VERSION 3.16)

project(local_vfs_test C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(local_vfs_test
    test_main.c
    ../local_vfs.c
    ../../mini9p/mini9p_protocol.c
)

target_include_directories(local_vfs_test PRIVATE
    ..
    ../../mini9p
)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(local_vfs_test PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endif()
