# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

# rovox_engine: everything that needs Tracktion/JUCE. It is an INTERFACE target on purpose: JUCE modules are
# compiled into whichever executable links them, so the sources below are compiled once per consumer (rovox,
# rovox_engine_tests) with identical flags (ccache makes the second one cheap).
add_library(rovox_engine INTERFACE)

target_sources(rovox_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/AudioSink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BrowserStream.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ChangeCollector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandBus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandsEdit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandsMidi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandsPlugins.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandsRouting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandsTransport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DeliveryService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EngineHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Gateway.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/HttpServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Import.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Peaks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PluginCatalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PluginHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PluginScan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Project.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProjectFormat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProjectManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RenderJobs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RppImport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/StockPlugins.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Templates.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimePos.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Transport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/WebSocket.cpp)

target_include_directories(rovox_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} ${ROVOX_LAME_INCLUDE})

target_compile_definitions(rovox_engine INTERFACE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1
    TRACKTION_LOG_DEVICES=0)

target_link_libraries(rovox_engine INTERFACE
    rovox_core
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    PkgConfig::HTTPLIB
    PkgConfig::OPUS
    ${ROVOX_LAME_LIB}
    "-latomic")
