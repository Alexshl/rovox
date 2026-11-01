# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

add_subdirectory(p1_headless)

# One multi-call binary for spikes P2..P5 (plugins, stream, mcp, bench): compiling
# JUCE + Tracktion once instead of four times.
find_package(PkgConfig REQUIRED)

juce_add_console_app(rovox_spikes
    PRODUCT_NAME "rovox_spikes"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(rovox_spikes PRIVATE
    main.cpp
    p2_plugins/plugins.cpp
    p3_browser_stream/stream.cpp
    p4_mcp/mcp.cpp
    p5_bench/bench.cpp)

target_include_directories(rovox_spikes PRIVATE common)

target_compile_definitions(rovox_spikes PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1   # MessageManager::runDispatchLoopUntil is used to pump events while waiting
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1
    TRACKTION_LOG_DEVICES=0)

target_link_libraries(rovox_spikes PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    PkgConfig::OPUS
    "-latomic")
