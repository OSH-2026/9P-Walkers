file(GLOB LUA_SRCS "${CMAKE_CURRENT_LIST_DIR}/../lua/*.c")

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
        "../mini9p/mini9p_client.c"
        "../mini9p/mini9p_protocol.c"
        "../shell/shell.c"
        "../web/http_server.c"
        "../web/websocket_shell.c"
        ${LUA_SRCS}
    PRIV_REQUIRES
        driver
        esp_http_server
        esp_timer
        spi_flash
    EMBED_TXTFILES
        "../web/index.html"
    INCLUDE_DIRS
        "."
        "../lua"
        "../mini9p"
        "../shell"
        "../web")
