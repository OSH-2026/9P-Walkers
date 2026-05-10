cmake_minimum_required(VERSION 3.16)

project(pwos_slave_host_tests C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(MINI9P_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../pwos-shared/mini9p)

add_executable(mini9p_server_test
    test_main.c
    ${MINI9P_ROOT}/mini9p_protocol.c
    ${MINI9P_ROOT}/mini9p_server.c
)

target_include_directories(mini9p_server_test PRIVATE
    ${MINI9P_ROOT}
)

add_executable(slave_uart_transport_host_test
    test_slave_uart_transport_host.c
    ../uart_transport.c
    ${MINI9P_ROOT}/mini9p_protocol.c
)

target_include_directories(slave_uart_transport_host_test PRIVATE
    ..
    stubs
    ${MINI9P_ROOT}
)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mini9p_server_test PRIVATE
        -Wall
        -Wextra
        -Werror
    )
    target_compile_options(slave_uart_transport_host_test PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endif()
