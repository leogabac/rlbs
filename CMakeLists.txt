cmake_minimum_required(VERSION 3.20)

project(
    rlbs
    VERSION 0.1.0
    DESCRIPTION "A lightweight batch scheduler for small research clusters"
    LANGUAGES CXX
)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)

# cmake renamed this target in 4.3, so give older cluster installs the new name
# too instead of making the minimum version line a lie
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

add_library(rlbs_logging
    src/logging/logger.cpp
)

target_include_directories(rlbs_logging
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_library(rlbs_core
    src/core/job.cpp
    src/core/node.cpp
    src/core/resources.cpp
    src/core/scheduler.cpp
    src/core/version.cpp
)

target_include_directories(rlbs_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_library(rlbs_execution
    src/execution/identity.cpp
    src/execution/process_runner.cpp
)

target_include_directories(rlbs_execution
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_library(rlbs_persistence
    src/persistence/database.cpp
    src/persistence/job_repository.cpp
    src/persistence/queue_repository.cpp
)

target_include_directories(rlbs_persistence
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rlbs_persistence
    PRIVATE
        rlbs_core
        SQLite3::SQLite3
)

add_library(rlbs_control
    src/control/authorization.cpp
    src/control/protocol.cpp
    src/control/unix_socket.cpp
)

target_include_directories(rlbs_control
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rlbs_control
    PUBLIC
        rlbs_core
        rlbs_persistence
)

add_library(rlbs_local
    src/local/coordinator.cpp
    src/local/output_spool.cpp
    src/local/runtime_environment.cpp
)

target_include_directories(rlbs_local
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rlbs_local
    PUBLIC
        rlbs_core
        rlbs_execution
        rlbs_logging
        rlbs_persistence
)

# cancel requests have to reach the coordinator that actually owns a running
# process. keep this after both targets exist so cmake sees a project target
target_link_libraries(rlbs_control
    PUBLIC
        rlbs_local
)

add_library(rlbs_daemon
    src/daemon/config.cpp
)

target_include_directories(rlbs_daemon
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rlbs_daemon
    PUBLIC
        rlbs_core
)

add_library(rlbs_cli
    src/cli/duration.cpp
    src/cli/native_script.cpp
    src/cli/pbs_script.cpp
    src/cli/query.cpp
    src/cli/queues.cpp
    src/cli/submit.cpp
)

target_include_directories(rlbs_cli
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(rlbs_cli
    PUBLIC
        rlbs_core
)

add_executable(rlbs
    apps/rlbs/main.cpp
)

target_link_libraries(rlbs
    PRIVATE
        rlbs_cli
        rlbs_control
        rlbs_core
)

# these stay real aliases instead of three tiny binaries that eventually drift
# apart because somebody fixed qsub and forgot the other submit path existed
add_custom_target(pbs_aliases ALL
    COMMAND ${CMAKE_COMMAND} -E rm -f
        $<TARGET_FILE_DIR:rlbs>/qsub
        $<TARGET_FILE_DIR:rlbs>/qstat
        $<TARGET_FILE_DIR:rlbs>/qdel
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        $<TARGET_FILE_NAME:rlbs> $<TARGET_FILE_DIR:rlbs>/qsub
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        $<TARGET_FILE_NAME:rlbs> $<TARGET_FILE_DIR:rlbs>/qstat
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        $<TARGET_FILE_NAME:rlbs> $<TARGET_FILE_DIR:rlbs>/qdel
    DEPENDS rlbs
)

add_executable(rlbsd
    apps/rlbsd/main.cpp
)

target_link_libraries(rlbsd
    PRIVATE
        rlbs_control
        rlbs_daemon
        rlbs_local
        rlbs_persistence
)

add_executable(rlbs-agent
    apps/rlbs-agent/main.cpp
)

target_link_libraries(rlbs-agent
    PRIVATE
        rlbs_core
)

install(TARGETS rlbs rlbsd rlbs-agent
    RUNTIME DESTINATION bin
)

# cmake has already made bin for the installed executables by this point. use
# relative links so moving a staged install does not leave three broken paths
install(CODE [[
    foreach(alias IN ITEMS qsub qstat qdel)
        set(alias_path "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/${alias}")
        file(REMOVE "${alias_path}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E create_symlink rlbs "${alias_path}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endforeach()
]])

include(CTest)

if(BUILD_TESTING)
    add_executable(rlbs_logging_tests
        tests/logging/logger_test.cpp
    )

    target_link_libraries(rlbs_logging_tests
        PRIVATE
            rlbs_logging
    )

    add_test(
        NAME rlbs_logging_tests
        COMMAND rlbs_logging_tests
    )

    add_executable(rlbs_core_tests
        tests/core/scheduler_test.cpp
    )

    target_link_libraries(rlbs_core_tests
        PRIVATE
            rlbs_core
    )

    add_test(
        NAME rlbs_core_tests
        COMMAND rlbs_core_tests
    )

    add_executable(rlbs_execution_tests
        tests/execution/process_runner_test.cpp
    )

    target_link_libraries(rlbs_execution_tests
        PRIVATE
            rlbs_execution
    )

    add_test(
        NAME rlbs_execution_tests
        COMMAND rlbs_execution_tests
    )

    add_executable(rlbs_persistence_tests
        tests/persistence/database_test.cpp
    )

    target_link_libraries(rlbs_persistence_tests
        PRIVATE
            rlbs_persistence
            SQLite3::SQLite3
    )

    add_test(
        NAME rlbs_persistence_tests
        COMMAND rlbs_persistence_tests
    )

    add_executable(rlbs_job_repository_tests
        tests/persistence/job_repository_test.cpp
    )

    target_link_libraries(rlbs_job_repository_tests
        PRIVATE
            rlbs_persistence
            SQLite3::SQLite3
    )

    add_test(
        NAME rlbs_job_repository_tests
        COMMAND rlbs_job_repository_tests
    )

    add_executable(rlbs_queue_repository_tests
        tests/persistence/queue_repository_test.cpp
    )

    target_link_libraries(rlbs_queue_repository_tests
        PRIVATE
            rlbs_persistence
            SQLite3::SQLite3
    )

    add_test(
        NAME rlbs_queue_repository_tests
        COMMAND rlbs_queue_repository_tests
    )

    add_executable(rlbs_local_tests
        tests/local/coordinator_test.cpp
    )

    target_link_libraries(rlbs_local_tests
        PRIVATE
            rlbs_local
    )

    add_test(
        NAME rlbs_local_tests
        COMMAND rlbs_local_tests
    )

    add_executable(rlbs_output_spool_tests
        tests/local/output_spool_test.cpp
    )

    target_link_libraries(rlbs_output_spool_tests
        PRIVATE
            rlbs_local
    )

    add_test(
        NAME rlbs_output_spool_tests
        COMMAND rlbs_output_spool_tests
    )

    add_executable(rlbs_daemon_tests
        tests/daemon/daemon_test.cpp
    )

    add_dependencies(rlbs_daemon_tests pbs_aliases)

    target_link_libraries(rlbs_daemon_tests
        PRIVATE
            rlbs_control
            rlbs_daemon
            rlbs_persistence
    )

    add_test(
        NAME rlbs_daemon_tests
        COMMAND rlbs_daemon_tests
            $<TARGET_FILE:rlbsd>
            $<TARGET_FILE:rlbs>
    )

    add_executable(rlbs_control_tests
        tests/control/authorization_test.cpp
        tests/control/protocol_test.cpp
    )

    target_link_libraries(rlbs_control_tests
        PRIVATE
            rlbs_control
    )

    add_test(
        NAME rlbs_control_tests
        COMMAND rlbs_control_tests
    )

    add_executable(rlbs_cli_tests
        tests/cli/submit_test.cpp
    )

    target_link_libraries(rlbs_cli_tests
        PRIVATE
            rlbs_cli
    )

    add_test(
        NAME rlbs_cli_tests
        COMMAND rlbs_cli_tests
    )

    add_executable(rlbs_pbs_script_tests
        tests/cli/pbs_script_test.cpp
    )

    target_link_libraries(rlbs_pbs_script_tests
        PRIVATE
            rlbs_cli
    )

    add_test(
        NAME rlbs_pbs_script_tests
        COMMAND rlbs_pbs_script_tests
    )

    add_executable(rlbs_native_script_tests
        tests/cli/native_script_test.cpp
    )

    target_link_libraries(rlbs_native_script_tests
        PRIVATE
            rlbs_cli
    )

    add_test(
        NAME rlbs_native_script_tests
        COMMAND rlbs_native_script_tests
    )

    add_executable(rlbs_query_cli_tests
        tests/cli/query_test.cpp
    )

    target_link_libraries(rlbs_query_cli_tests
        PRIVATE
            rlbs_cli
    )

    add_test(
        NAME rlbs_query_cli_tests
        COMMAND rlbs_query_cli_tests
    )

    add_executable(rlbs_queues_cli_tests
        tests/cli/queues_test.cpp
    )

    target_link_libraries(rlbs_queues_cli_tests
        PRIVATE
            rlbs_cli
    )

    add_test(
        NAME rlbs_queues_cli_tests
        COMMAND rlbs_queues_cli_tests
    )

    set_tests_properties(
        rlbs_execution_tests
        rlbs_local_tests
        rlbs_output_spool_tests
        rlbs_daemon_tests
        PROPERTIES
            TIMEOUT 10
    )

endif()
