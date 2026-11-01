# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

juce_add_console_app(rovox
    PRODUCT_NAME "rovox"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(rovox PRIVATE Main.cpp Cli.cpp)

target_link_libraries(rovox PRIVATE rovox_mcp CLI11::CLI11)
