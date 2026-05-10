cmake_minimum_required(VERSION 3.16)

project(pc_master_emulator C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(MINI9P_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mini9p)

add_executable(pc_master_emulator
    main.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

target_include_directories(pc_master_emulator PRIVATE
    ${MINI9P_ROOT}
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pc_master_emulator PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()
