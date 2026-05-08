cmake_minimum_required(VERSION 3.16)

project(pc_master_emulator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_executable(pc_master_emulator
    main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../pwos-slave/User/mini9p/mini9p_protocol.c
)

target_include_directories(pc_master_emulator PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../pwos-slave/User/mini9p
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pc_master_emulator PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()
