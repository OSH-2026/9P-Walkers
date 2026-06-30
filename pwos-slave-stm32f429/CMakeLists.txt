cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_PROJECT_NAME pwos-slave-stm32f429)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

enable_language(C ASM)

add_executable(${CMAKE_PROJECT_NAME})
add_subdirectory(cmake/stm32cubemx)

# pwos-slave source directories
set(PWOS_USER_DIR ${CMAKE_SOURCE_DIR}/User)
set(PWOS_SHARED_DIR ${CMAKE_SOURCE_DIR}/../pwos-shared)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Display / BSP
    User/bsp/bsp_sdram.c
    User/bsp/bsp_lcd.c
    User/bsp/display_sync.c
    User/gfx/gfx.c
    User/app/cube_demo.c
    # pwos-slave core
    ${PWOS_USER_DIR}/os/frame_pool.c
    ${PWOS_USER_DIR}/rtos/pwos_queues.c
    ${PWOS_USER_DIR}/rtos/pwos_tasks.c
    ${PWOS_USER_DIR}/drivers/uart_dma_port.c
    ${PWOS_USER_DIR}/link/port_manager.c
    ${PWOS_USER_DIR}/mesh2/node_control.c
    ${PWOS_USER_DIR}/service/service_runtime.c
    ${PWOS_USER_DIR}/service/rpc_service.c
    ${PWOS_USER_DIR}/service/job_service.c
    ${PWOS_USER_DIR}/compute/compute_worker.c
    ${PWOS_USER_DIR}/diag/fault_control.c
    ${PWOS_USER_DIR}/backend/local_vfs.c
    ${PWOS_USER_DIR}/fs/lfs.c
    ${PWOS_USER_DIR}/fs/lfs_util.c
    # Shared protocol
    ${PWOS_SHARED_DIR}/link/pwos_link_crc.c
    ${PWOS_SHARED_DIR}/link/pwos_link_frame.c
    ${PWOS_SHARED_DIR}/link/pwos_link_parser.c
    ${PWOS_SHARED_DIR}/mesh2/pwos_mesh2_control.c
    ${PWOS_SHARED_DIR}/mini9p/mini9p_protocol.c
    ${PWOS_SHARED_DIR}/mini9p/mini9p_server.c
    ${PWOS_SHARED_DIR}/rpc/pwos_rpc_protocol.c
    ${PWOS_SHARED_DIR}/job/pwos_job_protocol.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    User/bsp
    User/gfx
    User/app
    ${PWOS_USER_DIR}/drivers
    ${PWOS_USER_DIR}/link
    ${PWOS_USER_DIR}/mesh2
    ${PWOS_USER_DIR}/os
    ${PWOS_USER_DIR}/rtos
    ${PWOS_USER_DIR}/service
    ${PWOS_USER_DIR}/backend
    ${PWOS_USER_DIR}/diag
    ${PWOS_USER_DIR}/compute
    ${PWOS_USER_DIR}/fs
    ${PWOS_SHARED_DIR}/link
    ${PWOS_SHARED_DIR}/mesh2
    ${PWOS_SHARED_DIR}/mini9p
    ${PWOS_SHARED_DIR}/rpc
    ${PWOS_SHARED_DIR}/job
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:PWOS_ENABLE_FAULT_INJECTION=1>
)

list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
)

