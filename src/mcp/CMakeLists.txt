# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

# rovox_mcp: MCP server (JSON-RPC, tools, stdio and Streamable HTTP transports). INTERFACE like rovox_engine: the sources
# are compiled in the consumer so that Tracktion headers see identical flags.
add_library(rovox_mcp INTERFACE)

target_sources(rovox_mcp INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/McpHttp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/McpServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/McpStdio.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/McpTools.cpp)

target_include_directories(rovox_mcp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rovox_mcp INTERFACE rovox_engine)
