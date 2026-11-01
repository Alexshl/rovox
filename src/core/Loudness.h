// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Result.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace rovox
{

/** Level used instead of -infinity in reports (JSON has no infinity). */
inline constexpr double silenceDb = -200.0;

struct AudioStats
{
    double durationS = 0;
    double integratedLufs = silenceDb;      // ITU-R BS.1770-4, gated
    double shortTermMaxLufs = silenceDb;    // max of the 3 s window, sampled every 0.5 s
    double truePeakDbtp = silenceDb;        // 4x oversampled (libebur128)
    double samplePeakDbfs = silenceDb;
    double rmsDbfs = silenceDb;
    std::int64_t clippedSamples = 0;        // samples with |x| >= 1.0
    std::vector<std::pair<double, double>> silenceRanges;   // [start_s, end_s) below -60 dBFS for at least 1 s

    json toJson() const;
};

/** Streaming BS.1770-4 loudness + peak + clipping + silence analysis of planar float audio. */
class StatsAccumulator
{
public:
    StatsAccumulator (int numChannels, double sampleRate);
    ~StatsAccumulator();

    void add (const float* const* planar, int numFrames);
    AudioStats finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Convenience: whole buffer at once. */
AudioStats analyse (const std::vector<std::vector<float>>& planar, double sampleRate);

inline double linearToDb (double lin)   { return lin > 1.0e-10 ? 20.0 * std::log10 (lin) : silenceDb; }

} // namespace rovox
