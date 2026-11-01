# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

juce_add_console_app(p1_headless
    PRODUCT_NAME "p1_headless"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(p1_headless PRIVATE main.cpp)

target_compile_definitions(p1_headless PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_PLUGINHOST_VST3=0
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1
    TRACKTION_LOG_DEVICES=0)

target_link_libraries(p1_headless PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    "-latomic")
