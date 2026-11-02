// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER JS effects (JSFX) as a built-in Tracktion plugin ("rovoxjsfx"), run by ysfx (Apache-2.0, on WDL EEL2). The effect files are not part
// of Rovox: they are read from the directories of $ROVOX_JSFX_PATH (colon separated, default /jsfx; in a container mount the REAPER "Effects"
// folder read-only there). A JSFX plugin is identified by its path relative to such a root (the name that REAPER stores in a project, matched
// case-insensitively, with or without ".jsfx"); its state is that path plus the slider values (parameters s1..s64 of the plugin state).
#pragma once

#include "Tracktion.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rovox
{

/** Directories to look for JSFX files: $ROVOX_JSFX_PATH split at ':', or "/jsfx". Read on every call. */
std::vector<fs::path> jsfxRoots();

struct JsfxFile
{
    fs::path path;          // absolute
    fs::path root;          // the search directory that contains it
    std::string rel;        // path below the root with the real case and extension, '/' separated
};

/** A JSFX file by the name REAPER uses ("loser/Saturation"), case-insensitive. Never leaves the roots ("..", absolute paths are refused). */
std::optional<JsfxFile> findJsfx (const std::string& name);

/** plugin.catalog records ("plugin_uid" "jsfx:<rel>", format "JSFX", category "jsfx") of every JSFX below the roots. Scanned once per set of roots. */
std::vector<json> jsfxCatalogRecords();
std::optional<json> jsfxCatalogRecord (const std::string& uidOrName);
void jsfxRescan();
bool isJsfxUid (const std::string&);            // starts with "jsfx:"

/** Registers the plugin type with the engine's plugin manager; call once after the Engine exists. */
void registerJsfxPlugin (te::Engine&);

/** A new JSFX plugin in the edit (not yet in a chain). `sliders` maps slider numbers (1..64) to values; others stay at their defaults.
    Fails if the file is not below the roots or does not compile. */
Result<te::Plugin::Ptr> createJsfxPlugin (te::Edit&, const std::string& name, const std::map<int, double>& sliders = {});

/** Extra fields for fx.list / fx.params of a JSFX plugin: {"units", "jsfx_file", "latency_samples", "loaded", "load_error"}. Null for other plugins. */
json jsfxPluginExtras (te::Plugin&);

} // namespace rovox
