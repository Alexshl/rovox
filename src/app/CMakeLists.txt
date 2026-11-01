# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

juce_add_console_app(rovox
    PRODUCT_NAME "rovox"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(rovox PRIVATE Main.cpp)

target_compile_definitions(rovox PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(rovox PRIVATE
    rovox_core
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
