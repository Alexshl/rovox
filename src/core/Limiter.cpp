// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Limiter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

namespace rovox
{

namespace
{
constexpr int oversample = 4;
constexpr int tapsBefore = 7;       // interpolation uses x[n-7 .. n+8]
constexpr int tapsTotal = 16;

/** Windowed-sinc coefficients for the points n + 1/4, n + 2/4, n + 3/4. */
std::array<std::array<double, tapsTotal>, oversample - 1> makeInterpolator()
{
    std::array<std::array<double, tapsTotal>, oversample - 1> h {};
    const double pi = 3.14159265358979323846;

    for (int p = 1; p < oversample; ++p)
    {
        const double t = (double) p / oversample;
        double sum = 0.0;

        for (int k = 0; k < tapsTotal; ++k)
        {
            const double d = (double) (k - tapsBefore) - t;
            const double sinc = std::abs (d) < 1.0e-12 ? 1.0 : std::sin (pi * d) / (pi * d);
            const double w = 0.42 + 0.5 * std::cos (pi * d / 8.5) + 0.08 * std::cos (2.0 * pi * d / 8.5);     // Blackman, centred
            h[(size_t) p - 1][(size_t) k] = sinc * w;
            sum += sinc * w;
        }

        for (auto& c : h[(size_t) p - 1])
            c /= sum;
    }

    return h;
}

/** Peak of the (linked) signal, in linear amplitude, that each output sample must stay under: element n covers the sample n and the
    three interpolated points up to n + 1 (attributed to n and n + 1). */
std::vector<float> detectPeaks (const std::vector<std::vector<float>>& x, size_t frames)
{
    static const auto h = makeInterpolator();
    std::vector<float> peak (frames, 0.0f);

    for (const auto& ch : x)
    {
        for (size_t n = 0; n < frames; ++n)
        {
            float best = std::abs (ch[n]);

            if (n + 1 < frames)
            {
                for (int p = 0; p < oversample - 1; ++p)
                {
                    double acc = 0.0;

                    for (int k = 0; k < tapsTotal; ++k)
                    {
                        const long idx = (long) n + k - tapsBefore;

                        if (idx >= 0 && idx < (long) frames)
                            acc += h[(size_t) p][(size_t) k] * ch[(size_t) idx];
                    }

                    const float a = (float) std::abs (acc);

                    if (a > best)
                    {
                        best = a;
                    }
                }
            }

            peak[n] = std::max (peak[n], best);
        }
    }

    return peak;
}
} // namespace

LimiterResult limitTruePeak (std::vector<std::vector<float>>& planar, double sampleRate, double preGainDb, const LimiterSettings& settings)
{
    LimiterResult result;
    const size_t frames = planar.empty() ? 0 : planar.front().size();

    if (frames == 0)
        return result;

    const double preGain = std::pow (10.0, preGainDb / 20.0);
    const double ceiling = std::pow (10.0, settings.ceilingDbtp / 20.0);
    const size_t look = std::max<size_t> (1, (size_t) std::llround (settings.lookaheadMs * 0.001 * sampleRate));

    // required gain per sample; the interpolated points between n and n + 1 constrain both neighbours
    std::vector<float> peak = detectPeaks (planar, frames);
    std::vector<double> need (frames + look, 1.0);

    for (size_t n = 0; n < frames; ++n)
    {
        const double p = (double) peak[n] * preGain;
        const double g = p > ceiling ? ceiling / p : 1.0;
        need[n] = std::min (need[n], g);

        if (n > 0)
        {
            // the interpolated points after n - 1 are in peak[n - 1]; they bound n as well
            const double q = (double) peak[n - 1] * preGain;
            need[n] = std::min (need[n], q > ceiling ? ceiling / q : 1.0);
        }
    }

    peak.clear();
    peak.shrink_to_fit();

    // m[k] = min(need[k - look .. k]) (sliding minimum), s = instant attack + exponential release, g = moving average over look + 1 samples
    const size_t total = frames + look;
    const double relCoef = std::exp (-1.0 / (std::max (1.0, settings.releaseMs * 0.001 * sampleRate)));
    std::vector<double> smooth (total, 1.0);
    std::deque<size_t> window;
    double s = 1.0;

    for (size_t k = 0; k < total; ++k)
    {
        while (! window.empty() && need[window.back()] >= need[k])
            window.pop_back();

        window.push_back (k);

        while (window.front() + look < k)
            window.pop_front();

        const double m = need[window.front()];
        s = m < s ? m : relCoef * s + (1.0 - relCoef) * m;
        smooth[k] = s;
    }

    std::vector<double> envelope (total, 1.0);
    double running = 0.0;

    for (size_t k = 0; k < total; ++k)
    {
        running += smooth[k];

        if (k > look)
            running -= smooth[k - look - 1];

        // before the first full window the missing samples count as unity gain
        const size_t have = std::min (k, look) + 1;
        envelope[k] = (running + (double) (look + 1 - have)) / (double) (look + 1);
    }

    // out[j] = x[j] * pre * envelope[j + look]  (the envelope looks `look` samples ahead of the audio it protects)
    const double oneDb = std::pow (10.0, -1.0 / 20.0);
    size_t over = 0;
    double minGain = 1.0;

    for (size_t j = 0; j < frames; ++j)
    {
        const double e = envelope[j + look];
        const float g = (float) (preGain * e);

        for (auto& ch : planar)
            ch[j] *= g;

        minGain = std::min (minGain, e);

        if (e < oneDb)
            ++over;
    }

    result.maxGainReductionDb = minGain < 1.0 ? -20.0 * std::log10 (minGain) : 0.0;
    result.limitedOverOneDbPercent = 100.0 * (double) over / (double) frames;
    return result;
}

} // namespace rovox
