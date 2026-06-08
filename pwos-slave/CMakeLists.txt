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

option(PWOS_ENABLE_MINI9P_SERIAL "Enable F407 Mini9P mesh serial mode" OFF)
option(PWOS_ENABLE_LFS_SELFTEST "Populate littlefs debug fixtures on boot" ON)
option(PWOS_ENABLE_UART5_MESH "Enable UART5 mesh port; this conflicts with SDIO and disables SD-backed VFS" OFF)

if(PWOS_ENABLE_UART5_MESH)
    message(WARNING "PWOS_ENABLE_UART5_MESH uses PC12/PD2, which conflicts with SDIO; SD-backed VFS will be unavailable")
endif()

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
set(PWOS_USER_DIR ${CMAKE_SOURCE_DIR}/User)
set(PWOS_USER_MESH_DIR ${PWOS_USER_DIR}/mesh)
set(PWOS_SHARED_MESH_CLUSTER_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/cluster)
set(PWOS_SHARED_MESH_ENVELOPE_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/envelope)
set(PWOS_SHARED_MESH_PROCESSER_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/processer)
set(PWOS_SHARED_MESH_TRANSPORT_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/transport)
set(PWOS_SHARED_MESH_WIFI_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/wifi)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${PWOS_USER_DIR}/fs/lfs.c
    ${PWOS_USER_DIR}/fs/lfs_util.c
    ${PWOS_USER_DIR}/drivers/storage/sd_blockdev.cpp
    ${PWOS_USER_DIR}/drivers/storage/lfs_port.cpp
    ${PWOS_USER_DIR}/app/fs_selftest.c
    ${PWOS_USER_DIR}/app/mesh_diag.c
    ${PWOS_USER_DIR}/app/mesh_node_mini9p_init.c
    ${PWOS_USER_DIR}/app/pwos_log.c
    ${PWOS_USER_DIR}/app/vofa_firewater.c
    ${PWOS_USER_DIR}/backend/dev_vfs.c
    ${PWOS_USER_DIR}/backend/lfs_vfs.c
    ${PWOS_USER_DIR}/backend/node_vfs.c
    ${PWOS_USER_DIR}/backend/sys_vfs.c
    ${PWOS_USER_MESH_DIR}/mesh_node_service.c
    ${PWOS_SHARED_MESH_CLUSTER_DIR}/cluster.c
    ${PWOS_SHARED_MESH_ENVELOPE_DIR}/mesh_protocal.c
    ${PWOS_SHARED_MESH_PROCESSER_DIR}/mesh_processer.c
    ${PWOS_USER_MESH_DIR}/mesh_node_runtime.c
    ${PWOS_SHARED_MESH_TRANSPORT_DIR}/mesh_uart_transport.c
    ${PWOS_SHARED_MESH_WIFI_DIR}/mesh_wifi.c
    ${CMAKE_SOURCE_DIR}/../pwos-shared/mini9p/mini9p_protocol.c
    ${CMAKE_SOURCE_DIR}/../pwos-shared/mini9p/mini9p_server.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${PWOS_USER_DIR}/fs
    ${PWOS_USER_DIR}/drivers/storage
    ${PWOS_USER_DIR}/app
    ${PWOS_USER_DIR}/backend
    ${PWOS_USER_MESH_DIR}
    ${CMAKE_SOURCE_DIR}/../pwos-shared/mini9p
    ${PWOS_SHARED_MESH_CLUSTER_DIR}
    ${PWOS_SHARED_MESH_ENVELOPE_DIR}
    ${PWOS_SHARED_MESH_PROCESSER_DIR}
    ${PWOS_SHARED_MESH_TRANSPORT_DIR}
    ${PWOS_SHARED_MESH_WIFI_DIR}
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    MESH_UART_TRANSPORT_USE_STM32_HAL
)

if(PWOS_ENABLE_MINI9P_SERIAL)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_ENABLE_MINI9P_SERIAL)
endif()
if(PWOS_ENABLE_LFS_SELFTEST)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_ENABLE_LFS_SELFTEST)
endif()
if(PWOS_ENABLE_UART5_MESH)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_ENABLE_UART5_MESH)
endif()

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
