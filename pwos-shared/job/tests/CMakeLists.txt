cmake_minimum_required(VERSION 3.16)

project(pwos_job_protocol_tests C)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(JOB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(pwos_job_protocol_test
    test_job_protocol.c
    ${JOB_DIR}/pwos_job_protocol.c
)
target_include_directories(pwos_job_protocol_test PRIVATE ${JOB_DIR})
target_compile_options(pwos_job_protocol_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME pwos_job_protocol_test COMMAND pwos_job_protocol_test)
