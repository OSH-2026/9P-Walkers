cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME pwos-slave)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

option(PWOS_ENABLE_MINI9P_SERIAL "Use USART2 for Mini9P slave serial bring-up" OFF)
option(PWOS_BOARD_ZGT6 "Board has PF9 green LED (ZGT6)" OFF)
option(PWOS_ENABLE_LFS_SELFTEST "Populate littlefs debug fixtures on boot" ON)
option(PWOS_SKIP_LFS_MOUNT "Skip lfs_vfs mount during Mini9P board bring-up" OFF)

# Enable CMake support for ASM and C languages
enable_language(C CXX ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
set(PWOS_SHARED_MINI9P_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mini9p)
set(PWOS_SHARED_MESH_CLUSTER_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/cluster)
set(PWOS_SHARED_MESH_ENVELOPE_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/envelope)
set(PWOS_SHARED_MESH_PROCESSER_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/processer)
set(PWOS_SHARED_MESH_NODE_RUNTIME_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/node_runtime)
set(PWOS_SHARED_MESH_TRANSPORT_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/transport)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/User/fs/lfs.c
    ${CMAKE_SOURCE_DIR}/User/fs/lfs_util.c
    ${CMAKE_SOURCE_DIR}/User/drivers/storage/sd_blockdev.cpp
    ${CMAKE_SOURCE_DIR}/User/drivers/storage/lfs_port.cpp
    ${CMAKE_SOURCE_DIR}/User/app/fs_selftest.c
    ${CMAKE_SOURCE_DIR}/User/app/vofa_firewater.c
    ${CMAKE_SOURCE_DIR}/User/backend/dev_vfs.c
    ${CMAKE_SOURCE_DIR}/User/backend/lfs_vfs.c
    ${CMAKE_SOURCE_DIR}/User/backend/node_vfs.c
    ${CMAKE_SOURCE_DIR}/User/backend/sys_vfs.c
    ${CMAKE_SOURCE_DIR}/User/mini9p/mini9p_board_service.c
    ${PWOS_SHARED_MINI9P_DIR}/mini9p_protocol.c
    ${PWOS_SHARED_MINI9P_DIR}/mini9p_server.c
    ${PWOS_SHARED_MINI9P_DIR}/mini9p_service.c
    ${PWOS_SHARED_MESH_CLUSTER_DIR}/cluster.c
    ${PWOS_SHARED_MESH_ENVELOPE_DIR}/mesh_protocal.c
    ${PWOS_SHARED_MESH_PROCESSER_DIR}/mesh_processer.c
    ${PWOS_SHARED_MESH_NODE_RUNTIME_DIR}/mesh_node_runtime.c
    ${PWOS_SHARED_MESH_TRANSPORT_DIR}/mesh_uart_transport.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/User/fs
    ${CMAKE_SOURCE_DIR}/User/drivers/storage
    ${CMAKE_SOURCE_DIR}/User/app
    ${CMAKE_SOURCE_DIR}/User/backend
    ${CMAKE_SOURCE_DIR}/User/mini9p
    ${PWOS_SHARED_MINI9P_DIR}
    ${PWOS_SHARED_MESH_CLUSTER_DIR}
    ${PWOS_SHARED_MESH_ENVELOPE_DIR}
    ${PWOS_SHARED_MESH_PROCESSER_DIR}
    ${PWOS_SHARED_MESH_NODE_RUNTIME_DIR}
    ${PWOS_SHARED_MESH_TRANSPORT_DIR}
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    MESH_UART_TRANSPORT_USE_STM32_HAL
)

if(PWOS_ENABLE_MINI9P_SERIAL)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_ENABLE_MINI9P_SERIAL)
endif()
if(PWOS_BOARD_ZGT6)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_BOARD_ZGT6)
endif()
if(PWOS_ENABLE_LFS_SELFTEST)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_ENABLE_LFS_SELFTEST)
endif()
if(PWOS_SKIP_LFS_MOUNT)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_SKIP_LFS_MOUNT)
endif()

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
