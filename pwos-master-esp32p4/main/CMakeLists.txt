file(GLOB LUA_SRCS "${CMAKE_CURRENT_LIST_DIR}/../lua/*.c")
set(PWOS_SHARED_MINI9P_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mini9p")
set(PWOS_SHARED_MESH_CLUSTER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh/cluster")
set(PWOS_SHARED_MESH_ENVELOPE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh/envelope")
set(PWOS_SHARED_MESH_PROCESSER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh/processer")
set(PWOS_SHARED_MESH_NODE_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh/node_runtime")
set(PWOS_SHARED_MESH_TRANSPORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../pwos-shared/mesh/transport")

list(REMOVE_ITEM LUA_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/../lua/lua.c"
    "${CMAKE_CURRENT_LIST_DIR}/../lua/luac.c"
    "${CMAKE_CURRENT_LIST_DIR}/../lua/linit.c"
    "${CMAKE_CURRENT_LIST_DIR}/../lua/liolib.c"
    "${CMAKE_CURRENT_LIST_DIR}/../lua/loslib.c"
    "${CMAKE_CURRENT_LIST_DIR}/../lua/loadlib.c"
    "${CMAKE_CURRENT_LIST_DIR}/../lua/ldblib.c")

idf_component_register(
    SRCS
        "hello_world_main.c"
        "lua_bindings.c"
        "lua_vfs_bindings.c"
        "lua_port.c"
        "${PWOS_SHARED_MINI9P_DIR}/mini9p_client.c"
        "${PWOS_SHARED_MINI9P_DIR}/mini9p_protocol.c"
        "${PWOS_SHARED_MESH_ENVELOPE_DIR}/mesh_protocal.c"
        "${PWOS_SHARED_MESH_PROCESSER_DIR}/mesh_processer.c"
        "${PWOS_SHARED_MESH_CLUSTER_DIR}/cluster.c"
        "${PWOS_SHARED_MESH_TRANSPORT_DIR}/mesh_uart_transport.c"
        "../cluster/cluster_config.c"
        "../vfs_bridge/cluster_host_vfs.c"
        "../mesh/mesh_host_runtime.c"
        "../mesh/mesh_host_service.c"
        "../shell/shell.c"
        "../web/http_server.c"
        "../web/websocket_shell.c"
        "../web/wifi_softap.c"
        ${LUA_SRCS}
    EMBED_FILES
        "../web/index.html"
    PRIV_REQUIRES
        esp_timer
        spi_flash
        esp_http_server
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
    INCLUDE_DIRS
        "."
        "../lua"
        "${PWOS_SHARED_MINI9P_DIR}"
        "${PWOS_SHARED_MESH_CLUSTER_DIR}"
        "${PWOS_SHARED_MESH_ENVELOPE_DIR}"
        "${PWOS_SHARED_MESH_PROCESSER_DIR}"
        "${PWOS_SHARED_MESH_NODE_RUNTIME_DIR}"
        "${PWOS_SHARED_MESH_TRANSPORT_DIR}"
        "../mesh"
        "../cluster"
        "../vfs_bridge"
        "../shell"
        "../web")
