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
set(CMAKE_PROJECT_NAME pwos-slave-stm32f411)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

option(PWOS_LFS_BACKEND_RAM "Use RAM-backed littlefs for STM32F411 bring-up" ON)
option(PWOS_ENABLE_MINI9P_SERIAL "Enable STM32F411 Mini9P mesh serial bring-up" ON)
option(PWOS_ENABLE_LFS_SELFTEST "Populate littlefs debug fixtures on boot" ON)
option(PWOS_SKIP_LFS_MOUNT "Skip lfs_vfs mount during Mini9P board bring-up" OFF)

# Enable CMake support for ASM, C and C++ languages
enable_language(C CXX ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

set(PWOS_F411_USER_DIR ${CMAKE_SOURCE_DIR}/User)
set(PWOS_SLAVE_USER_DIR ${CMAKE_SOURCE_DIR}/../pwos-slave/User)
set(PWOS_SLAVE_USER_APP_DIR ${PWOS_SLAVE_USER_DIR}/app)
set(PWOS_SLAVE_USER_BACKEND_DIR ${PWOS_SLAVE_USER_DIR}/backend)
set(PWOS_SLAVE_USER_FS_DIR ${PWOS_SLAVE_USER_DIR}/fs)
set(PWOS_SLAVE_USER_MESH_DIR ${PWOS_SLAVE_USER_DIR}/mesh)
set(PWOS_SHARED_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared)
set(PWOS_SHARED_MINI9P_DIR ${PWOS_SHARED_DIR}/mini9p)
set(PWOS_SHARED_MESH_DIR ${PWOS_SHARED_DIR}/mesh)
set(PWOS_SHARED_MESH_CLUSTER_DIR ${PWOS_SHARED_MESH_DIR}/cluster)
set(PWOS_SHARED_MESH_ENVELOPE_DIR ${PWOS_SHARED_MESH_DIR}/envelope)
set(PWOS_SHARED_MESH_PROCESSER_DIR ${PWOS_SHARED_MESH_DIR}/processer)
set(PWOS_SHARED_MESH_TRANSPORT_DIR ${PWOS_SHARED_MESH_DIR}/transport)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${PWOS_SLAVE_USER_BACKEND_DIR}/dev_vfs.c
    ${PWOS_SLAVE_USER_BACKEND_DIR}/lfs_vfs.c
    ${PWOS_SLAVE_USER_BACKEND_DIR}/node_vfs.c
    ${PWOS_SLAVE_USER_BACKEND_DIR}/sys_vfs.c
    ${PWOS_SLAVE_USER_FS_DIR}/lfs.c
    ${PWOS_SLAVE_USER_FS_DIR}/lfs_util.c
    ${PWOS_F411_USER_DIR}/drivers/storage/lfs_port.cpp
    ${PWOS_SLAVE_USER_APP_DIR}/fs_selftest.c
    ${PWOS_F411_USER_DIR}/app/mesh_node_mini9p_init.c
    ${PWOS_SLAVE_USER_MESH_DIR}/mesh_node_service.c
    ${PWOS_SHARED_MESH_CLUSTER_DIR}/cluster.c
    ${PWOS_SHARED_MESH_ENVELOPE_DIR}/mesh_protocal.c
    ${PWOS_SHARED_MESH_PROCESSER_DIR}/mesh_processer.c
    ${PWOS_SLAVE_USER_MESH_DIR}/mesh_node_runtime.c
    ${PWOS_SHARED_MESH_TRANSPORT_DIR}/mesh_uart_transport.c
    ${PWOS_SHARED_MINI9P_DIR}/mini9p_protocol.c
    ${PWOS_SHARED_MINI9P_DIR}/mini9p_server.c
)

if(NOT PWOS_LFS_BACKEND_RAM)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PWOS_SLAVE_USER_DIR}/drivers/storage/sd_blockdev.cpp)
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${PWOS_SLAVE_USER_BACKEND_DIR}
    ${PWOS_SLAVE_USER_FS_DIR}
    ${PWOS_F411_USER_DIR}/drivers/storage
    ${PWOS_F411_USER_DIR}/app
    ${PWOS_SLAVE_USER_APP_DIR}
    ${PWOS_SLAVE_USER_MESH_DIR}
    ${PWOS_SHARED_MINI9P_DIR}
    ${PWOS_SHARED_MESH_CLUSTER_DIR}
    ${PWOS_SHARED_MESH_ENVELOPE_DIR}
    ${PWOS_SHARED_MESH_PROCESSER_DIR}
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
if(PWOS_LFS_BACKEND_RAM)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_LFS_PORT_USE_RAM)
else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PWOS_LFS_PORT_USE_SD)
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

# Post-build: generate .bin and .hex for flashing
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMENT "Generating .bin and .hex files for flashing"
)
