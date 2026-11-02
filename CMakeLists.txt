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

# ysfx (Apache-2.0): JSFX runtime on WDL EEL2 (zlib-style license), hosts REAPER JS effects (src/engine/JsfxPlugin). Headless: no graphics.
# EEL2 has native x86_64 and aarch64 code generators; -DROVOX_YSFX_PORTABLE=ON selects the portable (no JIT) interpreter instead.
option(ROVOX_YSFX_PORTABLE "Build ysfx/EEL2 without the machine-code generator" OFF)
set(ROVOX_YSFX_COMMIT "8077347ccf4115567aed81400281dca57acbb0cc")           # jpcima/ysfx
set(ROVOX_DR_LIBS_COMMIT "f13cbcfd06afe7287f99b1bb5982cefdf3d6a974")         # mackron/dr_libs, the submodule revision ysfx pins at that commit
include(FetchContent)
set(YSFX_GFX OFF CACHE BOOL "" FORCE)
set(YSFX_PLUGIN OFF CACHE BOOL "" FORCE)
set(YSFX_TESTS OFF CACHE BOOL "" FORCE)
set(YSFX_TOOLS OFF CACHE BOOL "" FORCE)
set(YSFX_SKIP_CHECKSUM ON CACHE BOOL "" FORCE)
set(YSFX_PORTABLE ${ROVOX_YSFX_PORTABLE} CACHE BOOL "" FORCE)
# Source archives of the pinned commits (a git clone of the dr_libs submodule is slow and not needed); the hashes make a changed archive fail loudly.
FetchContent_Declare(ysfx
    URL "https://github.com/jpcima/ysfx/archive/${ROVOX_YSFX_COMMIT}.tar.gz"
    URL_HASH SHA256=dd59dd86af6c612436b43c06590140b4706326f67d89ff3fffa8d9dc503f7b79)
FetchContent_Declare(ysfx_dr_libs
    URL "https://github.com/mackron/dr_libs/archive/${ROVOX_DR_LIBS_COMMIT}.tar.gz"
    URL_HASH SHA256=cc2695dbdd4902bed9bc8b7f7a0275fe53d657126f732bd28cfb2bd20cbb2701
    SOURCE_SUBDIR rovox-no-cmake-project)
FetchContent_MakeAvailable(ysfx_dr_libs ysfx)
# ysfx expects dr_libs as thirdparty/dr_libs inside its own tree; point the interface target at the separately fetched headers instead
set_property(TARGET dr_libs PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${ysfx_dr_libs_SOURCE_DIR}")

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
