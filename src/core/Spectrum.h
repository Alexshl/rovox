// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Band levels of a signal (task 240): Hann-windowed 8192-point FFT, 50 % overlap, averaged power per band.
#pragma once

#include "Result.h"

#include <vector>

namespace rovox
{

struct SpectrumBands
{
    double sub = -200, low = -200, lowMid = -200, highMid = -200, high = -200;      // dBFS (RMS of the band)

    json toJson() const;
};

class SpectrumAnalyser
{
public:
    explicit SpectrumAnalyser (double sampleRate);

    /** Mono samples (channels are averaged by the caller). */
    void add (const float* mono, int numFrames);
    SpectrumBands finish();

private:
    void processWindow();

    static constexpr int fftSize = 8192;
    double sampleRate_;
    std::vector<float> window_, buffer_;
    std::vector<double> power_;     // sum over windows of per-bin power (single sided, mean square)
    int filled_ = 0;
    long windows_ = 0;
};

/** In-place radix-2 FFT of interleaved re/im doubles; n must be a power of two. */
void fftInPlace (std::vector<double>& reim, int n);

} // namespace rovox
