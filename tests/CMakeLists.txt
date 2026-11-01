# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

add_executable(rovox_tests version_test.cpp)
target_link_libraries(rovox_tests PRIVATE rovox_core)
add_test(NAME rovox_tests COMMAND rovox_tests)

# Engine/command/render tests (need Tracktion): one binary, one ctest entry per suite.
juce_add_console_app(rovox_engine_tests
    PRODUCT_NAME "rovox_engine_tests"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(rovox_engine_tests PRIVATE
    TestMain.cpp
    TestSupport.cpp
    core_test.cpp
    engine_test.cpp
    project_test.cpp
    import_test.cpp
    render_test.cpp
    rpp_test.cpp
    upload_test.cpp
    cli_test.cpp)

target_link_libraries(rovox_engine_tests PRIVATE rovox_engine)

foreach(suite core engine project import render rpp upload cli)
    add_test(NAME ${suite} COMMAND rovox_engine_tests ${suite})
endforeach()

# the cli suite drives the real executable
add_dependencies(rovox_engine_tests rovox)
set_tests_properties(cli PROPERTIES ENVIRONMENT "ROVOX_CLI_BIN=$<TARGET_FILE:rovox>")
