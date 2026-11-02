// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Lookahead true-peak brickwall limiter for the rendered master (offline post-process, own DSP).
#pragma once

#include <cstddef>
#include <cstdint>
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

/** The same limiter as a stream (block by block, any block size, no allocation after prepare()). The output is the input delayed by
    latencySamples() (lookahead + the 8 samples the true-peak interpolation looks ahead) and is bit-identical for identical blocks. */
class StreamingLimiter
{
public:
    void prepare (double sampleRate, int numChannels, double lookaheadMs = 1.5);     // allocates
    void reset();
    /** `marginDb` lowers the working ceiling a little: the four-times oversampled detector can miss peaks between its points by a few hundredths of a dB. */
    void setParams (double preGainDb, double ceilingDbtp, double releaseMs, double marginDb = 0.0);
    int latencySamples() const noexcept         { return (int) look_ + 8; }

    /** In place, `numChannels` <= the prepared count. */
    void process (float* const* io, int numChannels, int numSamples);

    /** Largest gain reduction (dB, >= 0) seen in the last process() call. */
    double lastGainReductionDb() const noexcept { return lastReductionDb_; }

private:
    double sampleRate_ = 44100.0, pre_ = 1.0, ceiling_ = 0.891, relCoef_ = 0.99, lastReductionDb_ = 0.0, s_ = 1.0, running_ = 0.0, prevPeak_ = 0.0;
    size_t look_ = 1, ringSize_ = 64, window_ = 3, dqHead_ = 0, dqCount_ = 0;
    std::uint64_t count_ = 0;
    int channels_ = 0;
    std::vector<std::vector<float>> ring_;
    std::vector<double> need_, smooth_;
    std::vector<std::uint64_t> dq_;
};

} // namespace rovox
