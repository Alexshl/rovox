# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

add_library(rovox_core STATIC Version.cpp)
target_include_directories(rovox_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(rovox_core PRIVATE
    ROVOX_VERSION="${PROJECT_VERSION}"
    ROVOX_GIT_SHA="${ROVOX_GIT_SHA}"
    ROVOX_TRACKTION_VERSION="${ROVOX_TRACKTION_VERSION}")
