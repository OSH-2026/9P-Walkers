cmake_minimum_required(VERSION 3.22)
project(pwos_csc_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

set(PWOS_CSC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LTTIT_VENDOR ${PWOS_CSC_ROOT}/vendor/lttit)

add_executable(pwos_csc_ccnet_test
    test_ccnet.c
    ${PWOS_CSC_ROOT}/port/source/pwos_csc_heap_pc.c
    ${LTTIT_VENDOR}/CSC/ccnet/source/ccnet.c
    ${LTTIT_VENDOR}/lib/source/hashmap.c
    ${LTTIT_VENDOR}/lib/source/link_list.c
)

set_source_files_properties(
    ${LTTIT_VENDOR}/CSC/ccnet/source/ccnet.c
    PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-function"
)

target_include_directories(pwos_csc_ccnet_test PRIVATE
    ${PWOS_CSC_ROOT}/port/include
    ${LTTIT_VENDOR}/CSC
    ${LTTIT_VENDOR}/CSC/ccnet/include
    ${LTTIT_VENDOR}/lib/include
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pwos_csc_ccnet_test PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endif()

add_test(NAME pwos_csc_ccnet_test COMMAND pwos_csc_ccnet_test)

add_executable(pwos_csc_scp_smoke
    test_scp_smoke.c
    ${PWOS_CSC_ROOT}/port/source/pwos_csc_heap_pc.c
    ${PWOS_CSC_ROOT}/port/source/pwos_csc_time_pc.c
    ${LTTIT_VENDOR}/CSC/scp/source/scp.c
    ${LTTIT_VENDOR}/lib/source/hashmap.c
    ${LTTIT_VENDOR}/lib/source/link_list.c
    ${LTTIT_VENDOR}/lib/source/queue.c
    ${LTTIT_VENDOR}/lib/source/rbtree.c
)

set_source_files_properties(
    ${LTTIT_VENDOR}/CSC/scp/source/scp.c
    PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-unused-function;-Wno-unused-variable;-Wno-sign-compare;-Wno-conversion"
)

target_include_directories(pwos_csc_scp_smoke PRIVATE
    ${PWOS_CSC_ROOT}/port/include
    ${LTTIT_VENDOR}/CSC
    ${LTTIT_VENDOR}/CSC/scp/include
    ${LTTIT_VENDOR}/lib/include
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pwos_csc_scp_smoke PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endif()

add_test(NAME pwos_csc_scp_smoke COMMAND pwos_csc_scp_smoke)

add_executable(pwos_csc_rpc_smoke
    test_rpc_smoke.c
    ${PWOS_CSC_ROOT}/port/source/pwos_csc_heap_pc.c
    ${PWOS_CSC_ROOT}/port/source/pwos_csc_time_pc.c
    ${PWOS_CSC_ROOT}/port/source/pwos_csc_rpc_port_pc.c
    ${LTTIT_VENDOR}/CSC/ccrpc/source/rpc.c
    ${LTTIT_VENDOR}/lib/source/hashmap.c
    ${LTTIT_VENDOR}/lib/source/link_list.c
)

set_source_files_properties(
    ${LTTIT_VENDOR}/CSC/ccrpc/source/rpc.c
    PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-unused-function;-Wno-unused-variable;-Wno-cast-function-type;-Wno-sign-compare"
)

target_include_directories(pwos_csc_rpc_smoke PRIVATE
    ${PWOS_CSC_ROOT}/port/include
    ${LTTIT_VENDOR}/CSC
    ${LTTIT_VENDOR}/CSC/ccrpc/include
    ${LTTIT_VENDOR}/lib/include
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pwos_csc_rpc_smoke PRIVATE
        -Wall
        -Wextra
        -Werror
    )
endif()

add_test(NAME pwos_csc_rpc_smoke COMMAND pwos_csc_rpc_smoke)
