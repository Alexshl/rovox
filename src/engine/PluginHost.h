// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Plugin hosting outside the server process (task 410, variant B plus probes). Third-party plugin code can abort, segfault or hang.
// The server therefore never trusts a plugin it has not loaded in a child process first (probe), and renders of projects that
// contain external plugins run in a child process (`rovox render-child`). Whatever happens there, the server sees an error and a
// `plugin.crashed` event, not a dead process. Real-time playback (browser stream) still hosts plugins in-process (documented limit).
#pragma once

#include "PluginCatalog.h"
#include "Project.h"
#include "Render.h"
#include "Subprocess.h"

namespace rovox
{

/** Executable that provides the hidden `plugin-probe` and `render-child` commands: $ROVOX_HOST_BIN, else $ROVOX_CLI_BIN, else this
    process when it is `rovox`. Empty path if none is known. */
fs::path hostExecutable();

//==============================================================================
struct ProbeOutcome
{
    bool ok = false, crashed = false, timedOut = false;
    std::string error;          // load error / crash description
    json info = json::object(); // name, inputs, outputs, num_params, latency_samples, programs[], audible, peak_db ...
};

/** Loads the plugin in a child process, prepares it, pushes silent blocks through it (LV2 latency is only valid after the first block,
    spike P2) and, for an instrument, plays a note on every output bus. `presetsOnly` only lists programs. */
ProbeOutcome probePluginInChild (const juce::PluginDescription&, const fs::path& dataDir, bool presetsOnly = false, double timeoutS = 40);

/** Child entry: `rovox plugin-probe <desc.xml> [--presets-only]`. Prints one JSON line on stdout. */
int runPluginProbeChild (const std::string& descXmlFile, bool presetsOnly);

//==============================================================================
bool editHasExternalPlugins (te::Edit&);

/** A copy of the project (edit state + manifest + media symlink) under <data>/cache/render-child/<tag>/ that a child process can open
    without taking the project lock. Message thread. Empty path on failure. */
fs::path writeProjectSnapshot (Project&, const std::string& tag);

/** Renders the snapshot directory in a child process. Progress and cancellation follow `req.hooks`; a crash gives PLUGIN_CRASHED.
    `onCrash` gets {signal, stderr_tail} (the caller publishes plugin.crashed). */
Result<json> renderInChild (const fs::path& snapshotDir, const fs::path& dataDir, const RenderContext&, const RenderRequest&,
                            const std::function<void (const json&)>& onCrash = nullptr);

/** Child entry: `rovox render-child <snapshot dir> <request.json>`. Prints PROGRESS lines and one RESULT / ERROR line. */
int runRenderChild (const EngineConfig&, const std::string& snapshotDir, const std::string& requestFile);

//==============================================================================
/** Creates an ExternalPlugin for a catalog entry or an explicit {format, uid} (unknown to the catalog: created as is, it will report
    a load error). Probes the plugin in a child first; a crash blacklists it. Message thread. */
Result<te::Plugin::Ptr> createExternalPlugin (Project&, const json& pluginParam, json* probeInfo = nullptr);

/** After an undo / redo the tree holds the old plugin state while the live instance holds the new one; copy the tree back. */
void resyncPluginStates (te::Edit&);

} // namespace rovox
