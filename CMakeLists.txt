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

add_library(rlbs_core
    src/core/version.cpp
    src/core/resources.cpp
)

target_include_directories(rlbs_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_executable(rlbs
    apps/rlbs/main.cpp
)

target_link_libraries(rlbs
    PRIVATE
        rlbs_core
)

add_executable(rlbsd
    apps/rlbsd/main.cpp
)

target_link_libraries(rlbsd
    PRIVATE
        rlbs_core
)

add_executable(rlbs-agent
    apps/rlbs-agent/main.cpp
)

target_link_libraries(rlbs-agent
    PRIVATE
        rlbs_core
)
