// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Plugin scan (spike P2): scanning loads third-party code that can crash, so a real scan runs in a child process
// (the same executable, re-launched with a hidden `--plugin-scan-child` argument by the engine).
#pragma once

#include "EngineHost.h"

namespace rovox
{

/** Must be called at the top of main(), before anything else is created. If this process is a scan child it runs the scan
    protocol until the parent closes the pipe and returns true; main() should then return 0. */
bool runPluginScanChildIfRequested (int argc, char** argv);

/** Scans the LV2 / VST3 search paths (defaults: system dirs, `<pluginsDir>`, /plugins) into a JSON list.
    The engine must have been created with `scanPluginsOutOfProcess = true` for the scan to be isolated. */
json scanPlugins (EngineHost&, const std::vector<std::string>& formats = { "LV2", "VST3" }, const std::string& extraPath = {});

} // namespace rovox
