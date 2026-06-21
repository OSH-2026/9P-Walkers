cmake_minimum_required(VERSION 3.16)

project(pwos_mesh2_control_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(pwos_mesh2_control_test
    test_pwos_mesh2_control.c
    ../pwos_mesh2_control.c
    ../../link/pwos_link_crc.c
    ../../link/pwos_link_frame.c
)

target_include_directories(pwos_mesh2_control_test PRIVATE
    ..
    ../../link
)
