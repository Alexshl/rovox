// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Spectrum.h"

#include "Loudness.h"

#include <cmath>

namespace rovox
{

json SpectrumBands::toJson() const
{
    auto r = [] (double v) { return std::round (v * 10.0) / 10.0; };
    return { { "sub_20_60", r (sub) }, { "low_60_250", r (low) }, { "lowmid_250_2k", r (lowMid) }, { "highmid_2k_6k", r (highMid) }, { "high_6k_20k", r (high) } };
}

void fftInPlace (std::vector<double>& a, int n)
{
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;

        for (; j & bit; bit >>= 1)
            j ^= bit;

        j ^= bit;

        if (i < j)
        {
            std::swap (a[2 * (size_t) i], a[2 * (size_t) j]);
            std::swap (a[2 * (size_t) i + 1], a[2 * (size_t) j + 1]);
        }
    }

    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * M_PI / len;
        const double wr = std::cos (ang), wi = std::sin (ang);

        for (int i = 0; i < n; i += len)
        {
            double cr = 1.0, ci = 0.0;

            for (int k = 0; k < len / 2; ++k)
            {
                const size_t u = 2 * ((size_t) i + (size_t) k), v = 2 * ((size_t) i + (size_t) k + (size_t) len / 2);
                const double xr = a[v] * cr - a[v + 1] * ci, xi = a[v] * ci + a[v + 1] * cr;
                a[v] = a[u] - xr;
                a[v + 1] = a[u + 1] - xi;
                a[u] += xr;
                a[u + 1] += xi;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

SpectrumAnalyser::SpectrumAnalyser (double sampleRate) : sampleRate_ (sampleRate), window_ ((size_t) fftSize), buffer_ ((size_t) fftSize), power_ ((size_t) fftSize / 2 + 1, 0.0)
{
    for (int i = 0; i < fftSize; ++i)
        window_[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / fftSize));
}

void SpectrumAnalyser::add (const float* mono, int numFrames)
{
    for (int i = 0; i < numFrames; ++i)
    {
        buffer_[(size_t) filled_++] = mono[i];

        if (filled_ == fftSize)
        {
            processWindow();
            // 50 % overlap: keep the second half
            std::copy (buffer_.begin() + fftSize / 2, buffer_.end(), buffer_.begin());
            filled_ = fftSize / 2;
        }
    }
}

void SpectrumAnalyser::processWindow()
{
    std::vector<double> a ((size_t) fftSize * 2, 0.0);
    double sumW2 = 0;

    for (int i = 0; i < fftSize; ++i)
    {
        const double w = window_[(size_t) i];
        a[2 * (size_t) i] = buffer_[(size_t) i] * w;
        sumW2 += w * w;
    }

    fftInPlace (a, fftSize);

    // single-sided mean-square per bin, normalised so that the bins of a band sum to its mean-square (Parseval)
    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const double p = a[2 * (size_t) k] * a[2 * (size_t) k] + a[2 * (size_t) k + 1] * a[2 * (size_t) k + 1];
        const double scale = (k == 0 || k == fftSize / 2) ? 1.0 : 2.0;
        power_[(size_t) k] += scale * p / ((double) fftSize * sumW2);
    }

    ++windows_;
}

SpectrumBands SpectrumAnalyser::finish()
{
    SpectrumBands out;

    if (windows_ == 0)
        return out;

    auto band = [&] (double lo, double hi)
    {
        double sum = 0;

        for (int k = 1; k <= fftSize / 2; ++k)
        {
            const double f = k * sampleRate_ / fftSize;

            if (f >= lo && f < hi)
                sum += power_[(size_t) k] / (double) windows_;
        }

        return sum > 1.0e-20 ? 10.0 * std::log10 (sum) : silenceDb;
    };

    out.sub = band (20, 60);
    out.low = band (60, 250);
    out.lowMid = band (250, 2000);
    out.highMid = band (2000, 6000);
    out.high = band (6000, 20000);
    return out;
}

} // namespace rovox
