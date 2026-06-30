set(PWOS_SHARED_LINK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/link")
set(PWOS_SHARED_MESH2_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh2")
set(PWOS_SHARED_MINI9P_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mini9p")
set(PWOS_SHARED_RPC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/rpc")
set(PWOS_SHARED_JOB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/job")
set(PWOS_SHARED_HOST_RPC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/host_rpc")
set(PWOS_HOST_SESSIONS_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_sessions")
set(PWOS_HOST_API_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_api")
set(PWOS_HOST_SHELL_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_shell")
set(PWOS_HOST_NET_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_net")
set(PWOS_HOST_RPC_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_rpc")
set(PWOS_SLAVE_RPC_DIR "${CMAKE_CURRENT_LIST_DIR}/../slave_rpc")
set(PWOS_HOST_JOBS_DIR "${CMAKE_CURRENT_LIST_DIR}/../host_jobs")
set(PWOS_WEB_DIR "${CMAKE_CURRENT_LIST_DIR}/../web")

idf_component_register(
    SRCS
        "hello_world_main.c"
        "../coordinator_runtime/pwos_coordinator_runtime.c"
        "../host_coordinator/host_coordinator.c"
        "../host_sessions/session_manager.c"
        "../slave_rpc/rpc_client.c"
        "../host_rpc/host_rpc_service.c"
        "../host_rpc/host_rpc_peer_client.c"
        "../host_rpc/host_rpc_runtime.c"
        "../host_jobs/job_manager.c"
        "../host_jobs/job_command.c"
        "../host_api/cluster_vfs.c"
        "../host_api/host_observability.c"
        "../host_shell/command_service.c"
        "../host_shell/host_shell_runtime.c"
        "../host_net/lan_runtime.c"
        "../web/http_server.c"
        "../web/websocket_console.c"
        "${PWOS_SHARED_LINK_DIR}/pwos_link_crc.c"
        "${PWOS_SHARED_LINK_DIR}/pwos_link_frame.c"
        "${PWOS_SHARED_LINK_DIR}/pwos_link_parser.c"
        "${PWOS_SHARED_MESH2_DIR}/pwos_mesh2_control.c"
        "${PWOS_SHARED_MINI9P_DIR}/mini9p_protocol.c"
        "${PWOS_SHARED_MINI9P_DIR}/mini9p_client.c"
        "${PWOS_SHARED_RPC_DIR}/pwos_rpc_protocol.c"
        "${PWOS_SHARED_JOB_DIR}/pwos_job_protocol.c"
        "${PWOS_SHARED_HOST_RPC_DIR}/pwos_host_rpc_protocol.c"
        "${PWOS_SHARED_HOST_RPC_DIR}/pwos_host_election.c"
    EMBED_FILES
        "../web/index.html"
    PRIV_REQUIRES
        freertos
        esp_driver_uart
        esp_hw_support
        esp_system
        esp_timer
        esp_http_server
        esp_eth
        esp_event
        esp_netif
        lwip
        mdns
        nvs_flash
        ip101
        spi_flash
        heap
        log
    INCLUDE_DIRS
        "."
        "../coordinator_runtime"
        "../host_coordinator"
        "../host_sessions"
        "../host_api"
        "../host_shell"
        "../host_net"
        "../host_rpc"
        "../slave_rpc"
        "../host_jobs"
        "../web"
        "${PWOS_SHARED_LINK_DIR}"
        "${PWOS_SHARED_MESH2_DIR}"
        "${PWOS_SHARED_MINI9P_DIR}"
        "${PWOS_SHARED_RPC_DIR}"
        "${PWOS_SHARED_JOB_DIR}"
        "${PWOS_SHARED_HOST_RPC_DIR}")
