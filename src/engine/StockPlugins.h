// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Rovox stock effects: EQ, Comp, Gate, Limit, Delay, Verb. Built-in Tracktion plugins with their own DSP (src/core/StockFx, Limiter),
// automatable parameters in explicit units (Hz, dB, ms, ...). They are meant for the everyday jobs that the stock effects of a classic
// DAW do (EQ similar to ReaEQ, Comp to ReaComp, and so on), so projects and agents can work the same way.
#pragma once

#include "Tracktion.h"

#include <string>
#include <vector>

namespace rovox
{

struct StockInfo
{
    std::string uid;            // "rovox:eq" (catalog plugin_uid)
    std::string shortId;        // "eq"
    std::string name;           // "Rovox EQ"
    std::string xmlType;        // Tracktion plugin type, "rovoxeq"
    std::string similarTo;      // documentation only
    std::string description;
    std::vector<std::string> categories;
};

const std::vector<StockInfo>& stockPlugins();

/** By plugin_uid ("rovox:eq"), short id ("eq"), name ("Rovox EQ") or Tracktion type, any case. nullptr if none. */
const StockInfo* findStockPlugin (const std::string& key);

/** The Rovox stock effect that stands in for a REAPER stock FX, by the name in the RPP chunk (for example "VST: ReaEQ (Cockos)"). nullptr if none. */
const StockInfo* stockForReaperName (const std::string& fxName);

/** True for the stock entries that are instruments (rovox:drums): they take MIDI, not audio. */
bool isStockInstrument (const StockInfo&);

/** plugin.catalog record of a stock effect (format "Rovox", connector "local"). */
json stockCatalogRecord (const StockInfo&);

/** Registers the stock effects with the engine's plugin manager; call once after the Engine exists. */
void registerStockPlugins (te::Engine&);

/** Extra fields for fx.list / fx.params of a stock effect: {"units": {ident: unit}, "meters": {...}, "latency_samples": n}. Null for other plugins. */
json stockPluginExtras (te::Plugin&);

} // namespace rovox
