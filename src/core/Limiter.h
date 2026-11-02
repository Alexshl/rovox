// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Lookahead true-peak brickwall limiter for the rendered master (offline post-process, own DSP).
#pragma once

#include <vector>

namespace rovox
{

struct LimiterSettings
{
    double ceilingDbtp = -1.0;      // the true peak of the output stays at or below this
    double lookaheadMs = 1.5;
    double releaseMs = 80.0;
};

struct LimiterResult
{
    double maxGainReductionDb = 0.0;        // >= 0
    double limitedOverOneDbPercent = 0.0;   // share of the time (0..100) with more than 1 dB of gain reduction
};

/** Applies `preGainDb`, then limits `planar` in place. Peaks are detected 4x oversampled over all channels together (stereo link),
    the gain envelope reaches the needed reduction within the lookahead and recovers exponentially. The length does not change. */
LimiterResult limitTruePeak (std::vector<std::vector<float>>& planar, double sampleRate, double preGainDb, const LimiterSettings& = {});

} // namespace rovox
