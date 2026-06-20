cmake_minimum_required(VERSION 3.22)
project(pwos_link_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

set(PWOS_LINK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(pwos_link_test
    test_pwos_link.c
    ${PWOS_LINK_ROOT}/pwos_link_crc.c
    ${PWOS_LINK_ROOT}/pwos_link_frame.c
    ${PWOS_LINK_ROOT}/pwos_link_parser.c
)

target_include_directories(pwos_link_test PRIVATE
    ${PWOS_LINK_ROOT}
)

target_compile_definitions(pwos_link_test PRIVATE
    PWOS_LINK_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pwos_link_test PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endif()

add_test(NAME pwos_link_test COMMAND pwos_link_test)
