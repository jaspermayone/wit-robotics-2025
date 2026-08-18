# Host-side Foxglove tools for the Monster Book of Monsters battlebot.
#
# This is a standalone project. It is NOT part of the firmware build in the
# repository root, which cross-compiles for the Pico 2 W.
#
# There is no bridge here. The bot serves Foxglove itself, from
# `src/networking/foxglove_ws.c`. This project only builds demo data.
#
#   cmake -S . -B build
#   cmake --build build
#
# Produces:
#   build/generate_fake_mcap      simulated match recorder

cmake_minimum_required(VERSION 3.20)
project(battlebot_foxglove_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Put the executables directly in the build directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}")
endforeach()

include(cmake/FoxgloveSdk.cmake)

# Code shared with any future host tool.
add_library(battlebot_common STATIC
    src/json.cpp
    src/schemas.cpp
    src/telemetry.cpp
)
target_include_directories(battlebot_common PUBLIC src)
target_link_libraries(battlebot_common PUBLIC foxglove_cpp)

# The repository's MCAP directory, where the layouts and the sample recording
# live. The generator writes there by default, whatever directory it runs from.
target_compile_definitions(battlebot_common PUBLIC
    BATTLEBOT_DEFAULT_MCAP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../mcap")

if(MSVC)
    target_compile_options(battlebot_common PUBLIC /W4)
else()
    target_compile_options(battlebot_common PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(generate_fake_mcap src/generate_fake_mcap.cpp)
target_link_libraries(generate_fake_mcap PRIVATE battlebot_common)
