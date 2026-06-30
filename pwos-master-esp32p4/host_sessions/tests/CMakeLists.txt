cmake_minimum_required(VERSION 3.16)

project(pwos_host_session_tests C)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

set(HOST_SESSIONS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SLAVE_RPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../slave_rpc)
set(HOST_JOBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../host_jobs)
set(SHARED_MINI9P_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/mini9p)
set(SHARED_RPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/rpc)
set(SHARED_JOB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/job)

add_executable(pwos_host_session_test
    test_session_manager.c
    ${HOST_SESSIONS_DIR}/session_manager.c
    ${SLAVE_RPC_DIR}/rpc_client.c
    ${HOST_JOBS_DIR}/job_manager.c
    ${HOST_JOBS_DIR}/job_command.c
    ${SHARED_MINI9P_DIR}/mini9p_protocol.c
    ${SHARED_MINI9P_DIR}/mini9p_client.c
    ${SHARED_RPC_DIR}/pwos_rpc_protocol.c
    ${SHARED_JOB_DIR}/pwos_job_protocol.c
)

target_include_directories(pwos_host_session_test PRIVATE
    ${HOST_SESSIONS_DIR}
    ${SLAVE_RPC_DIR}
    ${HOST_JOBS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../pwos-shared/link
    ${SHARED_MINI9P_DIR}
    ${SHARED_RPC_DIR}
    ${SHARED_JOB_DIR}
)

target_compile_options(pwos_host_session_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_link_libraries(pwos_host_session_test PRIVATE Threads::Threads)

add_test(NAME pwos_host_session_test COMMAND pwos_host_session_test)
