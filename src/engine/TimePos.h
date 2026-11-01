// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// TimePos / Duration (task 210): the one way commands accept a time.
//   TimePos:  {"s": 12.5} | {"bar": 18, "beat": 1, "tick": 0} (bar and beat are 1-based, 960 ticks per quarter note)
//             | {"qn": 68.0} | {"marker": "mrk_..."}
//   Duration: {"s": 2.0} | {"bars": 2} | {"qn": 8.0}
// Responses always carry both seconds and bar.beat.tick.
#pragma once

#include "Project.h"

namespace rovox
{

/** JSON Schema fragments for the two types (the subset validator ignores oneOf; the parser is the real check). */
json timePosSchema();
json durationSchema();

Result<double> timePosToSeconds (Project&, const json& timePos);
/** `at` is where the duration starts (bars and quarter notes depend on the tempo map there). */
Result<double> durationToSeconds (Project&, const json& duration, double atSeconds);

/** Reads `<key>_s` (seconds) or `<key>` (a TimePos) from `params`. Returns nullopt if neither is given. */
Result<std::optional<double>> readTime (Project&, const json& params, const std::string& key);

std::string barBeatString (te::Edit&, double seconds);          // "40.1.000"
json describeTime (te::Edit&, double seconds);                  // {"s":..,"bar_beat":"40.1.000","qn":..}

} // namespace rovox
