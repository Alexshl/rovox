// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Translation of REAPER stock FX settings into Rovox stock effect parameters. The input is what the ReaScript exporter
// (tools/reaper/rovox_export_fx.lua) writes: per FX the parameter list with the normalized value and the text REAPER shows for it
// ("-12.0 dB", "1.50 kHz", "3.0:1", "-inf dB"). Only the formatted text is interpreted (units are part of it); the normalized value is the
// fallback for unit-less fractions. Pure functions, no engine dependency.
#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rovox
{

struct ReaperFxParam
{
    int index = 0;
    std::string name, formatted;
    double normalized = 0.0;
};

/** One FX of the sidecar file `<project>.rovoxfx.json`. */
struct ReaperFxEntry
{
    std::string name, guid;
    bool enabled = true;
    std::vector<ReaperFxParam> params;
    std::map<std::string, std::string> config;      // named config values (ReaEQ: BANDTYPE0, BANDENABLED0, ...)
};

ReaperFxEntry parseReaperFxEntry (const nlohmann::json&);

/** "{1A2B...}" in any case, with or without braces, to a comparable key (hex digits only, lower case). */
std::string normaliseReaperGuid (const std::string&);

struct TranslatedParam
{
    std::string from, formatted;        // REAPER parameter name and the text REAPER shows
    std::string to;                     // Rovox parameter id ("threshold", "b2_freq")
    double value = 0.0;                 // in the unit of the Rovox parameter
    std::string note;
};

struct UntranslatedParam
{
    std::string from, formatted, reason;
};

struct FxTranslation
{
    std::vector<TranslatedParam> translated;
    std::vector<UntranslatedParam> untranslated;
};

/** `stockShortId` is "eq", "comp", "gate", "limit", "delay" or "verb" (StockInfo::shortId). Values are not clamped to the Rovox ranges. */
FxTranslation translateReaperFx (const std::string& stockShortId, const ReaperFxEntry&);

/** A number with its unit from the text REAPER shows. */
struct ParsedValue
{
    bool ok = false;
    double value = 0.0;
    std::string unit;               // lower case: "db", "hz", "khz", "ms", "s", "%", "oct", ":1", ""
    bool infinite = false;          // "-inf" / "inf": value is -1 / +1
};

ParsedValue parseReaperValue (const std::string& formatted);

/** Bandwidth in octaves that gives this Q in the Rovox EQ (inverse of the design formula in StockFx). */
double bandwidthOctForQ (double q);

} // namespace rovox
