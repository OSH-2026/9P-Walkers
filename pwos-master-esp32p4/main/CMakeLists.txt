set(PWOS_SHARED_LINK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/link")
set(PWOS_SHARED_MESH2_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh2")
set(PWOS_SHARED_MINI9P_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mini9p")
set(PWOS_HOST_SESSIONS_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_sessions")
set(PWOS_HOST_API_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_api")

idf_component_register(
    SRCS
        "hello_world_main.c"
        "../coordinator_runtime/pwos_coordinator_runtime.c"
        "../host_coordinator/host_coordinator.c"
        "../host_sessions/session_manager.c"
        "../host_api/cluster_vfs.c"
        "${PWOS_SHARED_LINK_DIR}/pwos_link_crc.c"
        "${PWOS_SHARED_LINK_DIR}/pwos_link_frame.c"
        "${PWOS_SHARED_LINK_DIR}/pwos_link_parser.c"
        "${PWOS_SHARED_MESH2_DIR}/pwos_mesh2_control.c"
        "${PWOS_SHARED_MINI9P_DIR}/mini9p_protocol.c"
        "${PWOS_SHARED_MINI9P_DIR}/mini9p_client.c"
    PRIV_REQUIRES
        freertos
        esp_driver_uart
        esp_hw_support
        esp_system
        spi_flash
        heap
        log
    INCLUDE_DIRS
        "."
        "../coordinator_runtime"
        "../host_coordinator"
        "../host_sessions"
        "../host_api"
        "${PWOS_SHARED_LINK_DIR}"
        "${PWOS_SHARED_MESH2_DIR}"
        "${PWOS_SHARED_MINI9P_DIR}")
