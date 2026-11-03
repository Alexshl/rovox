// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The built-in drum sampler ("rovox:drums", Tracktion plugin type "rovoxdrums"): a General MIDI drum instrument that plays the MIDI drum parts written
// for MT-PowerDrumKit unchanged. The sound engine is src/core/DrumKit; the samples of the kit (property "kit" of the plugin state, default "drskit") are
// found by DrumKitLibrary (ROVOX_DRUMKITS_PATH, then the object store, then the local cache) and decoded in a loader thread. Without a kit the plugin
// is silent and says why in its state (drumsPluginExtras), it never crashes.
#pragma once

#include "Tracktion.h"

namespace rovox
{

inline constexpr const char* drumsXmlType = "rovoxdrums";
inline constexpr const char* defaultDrumKit = "drskit";

/** Registers the plugin type with the engine's plugin manager; call once after the Engine exists. */
void registerDrumsPlugin (te::Engine&);

/** State of a drum sampler for fx.list / fx.add: {"kit", "status": "resolving|loading|ready|error", "message", "error", "source", "name", "license", "attribution",
    "source_url", "pieces", "samples", "unmapped_notes"}. `waitS` > 0 waits (at most that long) until the kit is found and parsed or failed. Null for other plugins. */
json drumsPluginExtras (te::Plugin&, double waitS = 0.0);

/** The credit line the UI shows next to the plugin. */
inline constexpr const char* drumsAttribution = "Drum samples: DRSKit by the DrumGizmo team and Jes Eiler (DRSDrums), CC BY 4.0";

} // namespace rovox
