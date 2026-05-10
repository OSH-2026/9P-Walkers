file(GLOB LUA_SRCS "${CMAKE_CURRENT_LIST_DIR}/../lua/*.c")
set(PWOS_MINI9P_DIR "${CMAKE_CURRENT_LIST_DIR}/../mini9p")

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
        "lua_port.c"
        "${PWOS_MINI9P_DIR}/mini9p_client.c"
        "${PWOS_MINI9P_DIR}/mini9p_protocol.c"
        "../transport/uart_transport.c"
        "../vfs_bridge/cluster_config.c"
        "../vfs_bridge/cluster_vfs.c"
        "../shell/shell.c"
        ${LUA_SRCS}
    PRIV_REQUIRES
        esp_timer
        spi_flash
    INCLUDE_DIRS
        "."
        "../lua"
        "${PWOS_MINI9P_DIR}"
        "../transport"
        "../vfs_bridge"
        "../shell")
