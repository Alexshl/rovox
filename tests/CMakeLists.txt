# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

add_executable(rovox_tests version_test.cpp)
target_link_libraries(rovox_tests PRIVATE rovox_core)
add_test(NAME rovox_tests COMMAND rovox_tests)
