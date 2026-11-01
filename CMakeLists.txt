# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

cmake_minimum_required(VERSION 3.22)

project(rovox VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ROVOX_BUILD_TESTS  "Build the unit tests" ON)
option(ROVOX_BUILD_SPIKES "Build the throwaway spike programs under spikes/" OFF)

set(ROVOX_TE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracktion_engine")
if(NOT EXISTS "${ROVOX_TE_DIR}/modules/juce/CMakeLists.txt")
    message(FATAL_ERROR "Submodules are missing: run 'git submodule update --init --recursive'")
endif()

# Version info: Rovox git SHA (env override is used in Docker builds, where .git is absent)
if(DEFINED ENV{ROVOX_GIT_SHA} AND NOT "$ENV{ROVOX_GIT_SHA}" STREQUAL "")
    set(ROVOX_GIT_SHA "$ENV{ROVOX_GIT_SHA}")
else()
    find_package(Git QUIET)
    set(ROVOX_GIT_SHA "unknown")
    if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE ROVOX_GIT_SHA
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
endif()

file(READ "${ROVOX_TE_DIR}/VERSION.md" ROVOX_TRACKTION_VERSION)
string(STRIP "${ROVOX_TRACKTION_VERSION}" ROVOX_TRACKTION_VERSION)

# JUCE comes from Tracktion's nested submodule so both always match
add_subdirectory("${ROVOX_TE_DIR}/modules/juce" third_party_juce)
add_subdirectory("${ROVOX_TE_DIR}/modules" third_party_tracktion)

find_package(nlohmann_json 3.9 REQUIRED)
find_package(CLI11 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
pkg_check_modules(EBUR128 REQUIRED IMPORTED_TARGET libebur128)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(TOMLPP REQUIRED IMPORTED_TARGET tomlplusplus)
find_library(ROVOX_LAME_LIB NAMES mp3lame REQUIRED)
find_path(ROVOX_LAME_INCLUDE lame/lame.h REQUIRED)

add_subdirectory(src/core)
add_subdirectory(src/engine)
add_subdirectory(src/mcp)
add_subdirectory(src/app)

if(ROVOX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ROVOX_BUILD_SPIKES)
    add_subdirectory(spikes)
endif()
