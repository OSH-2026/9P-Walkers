set(PWOS_P4_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-master-esp32p4")
set(PWOS_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared")

idf_component_register(
    SRCS
        "main.cpp"
        "../host_net/lan_runtime.c"
        "../inference/llm_engine.cpp"
        "../inference/inference_runtime.cpp"
        "../inference/dist_inference_service.c"
        "${PWOS_P4_DIR}/coordinator_runtime/pwos_coordinator_runtime.c"
        "${PWOS_P4_DIR}/host_coordinator/host_coordinator.c"
        "${PWOS_P4_DIR}/host_sessions/session_manager.c"
        "${PWOS_P4_DIR}/slave_rpc/rpc_client.c"
        "${PWOS_P4_DIR}/host_rpc/host_rpc_service.c"
        "${PWOS_P4_DIR}/host_rpc/host_rpc_peer_client.c"
        "${PWOS_P4_DIR}/host_rpc/host_rpc_runtime.c"
        "${PWOS_P4_DIR}/host_jobs/job_manager.c"
        "${PWOS_P4_DIR}/host_jobs/job_command.c"
        "${PWOS_P4_DIR}/host_api/cluster_vfs.c"
        "${PWOS_SHARED_DIR}/link/pwos_link_crc.c"
        "${PWOS_SHARED_DIR}/link/pwos_link_frame.c"
        "${PWOS_SHARED_DIR}/link/pwos_link_parser.c"
        "${PWOS_SHARED_DIR}/mesh2/pwos_mesh2_control.c"
        "${PWOS_SHARED_DIR}/mini9p/mini9p_protocol.c"
        "${PWOS_SHARED_DIR}/mini9p/mini9p_client.c"
        "${PWOS_SHARED_DIR}/rpc/pwos_rpc_protocol.c"
        "${PWOS_SHARED_DIR}/job/pwos_job_protocol.c"
        "${PWOS_SHARED_DIR}/host_rpc/pwos_host_rpc_protocol.c"
        "${PWOS_SHARED_DIR}/host_rpc/pwos_host_election.c"
    PRIV_REQUIRES
        freertos
        esp_driver_uart
        esp_hw_support
        esp_system
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        lwip
        mdns
        nvs_flash
        spi_flash
        spiffs
        heap
        log
    INCLUDE_DIRS
        "."
        "../host_net"
        "../inference"
        "${PWOS_P4_DIR}/coordinator_runtime"
        "${PWOS_P4_DIR}/host_coordinator"
        "${PWOS_P4_DIR}/host_sessions"
        "${PWOS_P4_DIR}/slave_rpc"
        "${PWOS_P4_DIR}/host_rpc"
        "${PWOS_P4_DIR}/host_jobs"
        "${PWOS_P4_DIR}/host_api"
        "${PWOS_SHARED_DIR}/link"
        "${PWOS_SHARED_DIR}/mesh2"
        "${PWOS_SHARED_DIR}/mini9p"
        "${PWOS_SHARED_DIR}/rpc"
        "${PWOS_SHARED_DIR}/job"
        "${PWOS_SHARED_DIR}/host_rpc")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PWOS_COORDINATOR_UART_PORT=${CONFIG_PWOS_S3_UART_PORT}
    PWOS_COORDINATOR_UART_TX_PIN=${CONFIG_PWOS_S3_UART_TX_GPIO}
    PWOS_COORDINATOR_UART_RX_PIN=${CONFIG_PWOS_S3_UART_RX_GPIO}
    PWOS_COORDINATOR_UART_BAUD_RATE=${CONFIG_PWOS_S3_UART_BAUD_RATE})

spiffs_create_partition_image(model "${CMAKE_CURRENT_LIST_DIR}/../model" FLASH_IN_PROJECT)
