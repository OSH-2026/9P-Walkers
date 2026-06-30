cmake_minimum_required(VERSION 3.16)

project(local_vfs_test C)

enable_testing()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(MINI9P_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../pwos-shared/mini9p)

add_executable(local_vfs_test
    test_main.c
    ../local_vfs.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

target_include_directories(local_vfs_test PRIVATE
    ..
    ${MINI9P_ROOT}
)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(local_vfs_test PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endif()

add_test(NAME local_vfs_test COMMAND local_vfs_test)
