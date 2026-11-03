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
    cli_test.cpp
    delivery_test.cpp
    storage_test.cpp
    mcp_test.cpp
    plugin_test.cpp
    jsfx_test.cpp
    stock_test.cpp
    drums_test.cpp
    transport_test.cpp)

target_link_libraries(rovox_engine_tests PRIVATE rovox_mcp)

foreach(suite core engine project import render rpp upload cli mcp transport web stock drums jsfx plugins midi routing fx templates projects delivery storage)
    add_test(NAME ${suite} COMMAND rovox_engine_tests ${suite})
endforeach()

# the cli suite drives the real executable
add_dependencies(rovox_engine_tests rovox)
set_tests_properties(cli web PROPERTIES ENVIRONMENT "ROVOX_CLI_BIN=$<TARGET_FILE:rovox>")
# the plugin suites start plugin-probe / render-child helper processes (the executable) and use the fixture plugins built below
set_tests_properties(plugins midi routing fx templates projects mcp PROPERTIES
    ENVIRONMENT "ROVOX_CLI_BIN=$<TARGET_FILE:rovox>;ROVOX_TEST_LV2_DIR=${CMAKE_CURRENT_BINARY_DIR}/lv2")
set_tests_properties(transport PROPERTIES ENVIRONMENT "ROVOX_TEST_AUDIO=browser")
# the JSFX suite runs the effects written for the tests
set_tests_properties(jsfx PROPERTIES ENVIRONMENT "ROVOX_CLI_BIN=$<TARGET_FILE:rovox>;ROVOX_TEST_JSFX_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jsfx")

# Test fixture plugins (LV2 effects that crash on purpose), built into <build>/tests/lv2/rovox-test.lv2
add_library(rovox_test_lv2 MODULE fixtures/rovox-test.lv2/crash.c)
set_target_properties(rovox_test_lv2 PROPERTIES PREFIX "" OUTPUT_NAME crash LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lv2/rovox-test.lv2)
configure_file(fixtures/rovox-test.lv2/manifest.ttl ${CMAKE_CURRENT_BINARY_DIR}/lv2/rovox-test.lv2/manifest.ttl COPYONLY)
configure_file(fixtures/rovox-test.lv2/crash.ttl ${CMAKE_CURRENT_BINARY_DIR}/lv2/rovox-test.lv2/crash.ttl COPYONLY)
add_dependencies(rovox_engine_tests rovox_test_lv2)
