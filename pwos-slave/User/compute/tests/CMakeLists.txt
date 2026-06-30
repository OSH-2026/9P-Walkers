cmake_minimum_required(VERSION 3.16)

project(pwos_compute_worker_tests C)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(COMPUTE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(JOB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../pwos-shared/job)
set(RENDER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../pwos-shared/render)

add_executable(pwos_compute_worker_test
    test_compute_worker.c
    ${COMPUTE_DIR}/compute_worker.c
    ${JOB_DIR}/pwos_job_protocol.c
    ${RENDER_DIR}/pwos_smallpt.c
)
target_include_directories(pwos_compute_worker_test PRIVATE
    ${COMPUTE_DIR} ${JOB_DIR} ${RENDER_DIR})
target_link_libraries(pwos_compute_worker_test PRIVATE m)
target_compile_options(pwos_compute_worker_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME pwos_compute_worker_test COMMAND pwos_compute_worker_test)
