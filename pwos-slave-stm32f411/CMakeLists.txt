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

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

set(PWOS_SHARED_MINI9P_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mini9p)
set(PWOS_SHARED_MESH_CLUSTER_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/cluster)
set(PWOS_SHARED_MESH_ENVELOPE_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/envelope)
set(PWOS_SHARED_MESH_PROCESSER_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/processer)
set(PWOS_SHARED_MESH_NODE_RUNTIME_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/node_runtime)
set(PWOS_SHARED_MESH_TRANSPORT_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared/mesh/transport)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    User/backend/local_vfs.c
    User/mini9p/mini9p_service.c
    ${PWOS_SHARED_MINI9P_DIR}/mini9p_protocol.c
    ${PWOS_SHARED_MINI9P_DIR}/mini9p_server.c
    ${PWOS_SHARED_MESH_CLUSTER_DIR}/cluster.c
    ${PWOS_SHARED_MESH_ENVELOPE_DIR}/mesh_protocal.c
    ${PWOS_SHARED_MESH_PROCESSER_DIR}/mesh_processer.c
    ${PWOS_SHARED_MESH_NODE_RUNTIME_DIR}/mesh_node_runtime.c
    ${PWOS_SHARED_MESH_TRANSPORT_DIR}/mesh_uart_transport.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    User/backend
    User/mini9p
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
