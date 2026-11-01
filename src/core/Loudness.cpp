// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Loudness.h"

#include <ebur128.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rovox
{

namespace
{
double finiteOrSilence (double v)
{
    return std::isfinite (v) ? std::max (v, silenceDb) : silenceDb;
}

double round3 (double v)    { return std::round (v * 1000.0) / 1000.0; }
} // namespace

json AudioStats::toJson() const
{
    json ranges = json::array();

    for (auto& r : silenceRanges)
        ranges.push_back ({ { "start_s", round3 (r.first) }, { "end_s", round3 (r.second) } });

    return { { "duration_s", round3 (durationS) },
             { "integrated_lufs", round3 (integratedLufs) },
             { "short_term_max_lufs", round3 (shortTermMaxLufs) },
             { "true_peak_dbtp", round3 (truePeakDbtp) },
             { "sample_peak_dbfs", round3 (samplePeakDbfs) },
             { "rms_dbfs", round3 (rmsDbfs) },
             { "clipped_samples", clippedSamples },
             { "silence_ranges", ranges } };
}

struct StatsAccumulator::Impl
{
    Impl (int ch, double sr) : channels (ch), sampleRate (sr)
    {
        state = ebur128_init ((unsigned) ch, (unsigned long) sr, EBUR128_MODE_I | EBUR128_MODE_S | EBUR128_MODE_SAMPLE_PEAK | EBUR128_MODE_TRUE_PEAK);

        if (state == nullptr)
            throw std::runtime_error ("ebur128_init failed");
    }

    ~Impl() { ebur128_destroy (&state); }

    int channels;
    double sampleRate;
    ebur128_state* state = nullptr;

    std::int64_t frames = 0, clipped = 0, framesSinceShortTerm = 0, silenceStart = -1;
    double sumSquares = 0;
    double peak = 0;
    double shortTermMax = -HUGE_VAL;
    std::vector<std::pair<double, double>> silence;
    std::vector<float> scratch;

    static constexpr double silenceThreshold = 0.001;       // -60 dBFS

    void closeSilence (std::int64_t endFrame)
    {
        if (silenceStart >= 0 && (double) (endFrame - silenceStart) >= sampleRate)
            silence.emplace_back ((double) silenceStart / sampleRate, (double) endFrame / sampleRate);

        silenceStart = -1;
    }
};

StatsAccumulator::StatsAccumulator (int numChannels, double sampleRate) : impl_ (std::make_unique<Impl> (numChannels, sampleRate)) {}
StatsAccumulator::~StatsAccumulator() = default;

void StatsAccumulator::add (const float* const* planar, int numFrames)
{
    auto& s = *impl_;

    if (numFrames <= 0)
        return;

    // libebur128 1.2.x takes interleaved frames only
    s.scratch.resize ((size_t) numFrames * (size_t) s.channels);

    for (int c = 0; c < s.channels; ++c)
        for (int i = 0; i < numFrames; ++i)
            s.scratch[(size_t) i * (size_t) s.channels + (size_t) c] = planar[c][i];

    ebur128_add_frames_float (s.state, s.scratch.data(), (size_t) numFrames);

    for (int i = 0; i < numFrames; ++i)
    {
        double framePeak = 0;

        for (int c = 0; c < s.channels; ++c)
        {
            const double v = planar[c][i];
            const double a = std::abs (v);
            s.sumSquares += v * v;
            framePeak = std::max (framePeak, a);

            if (a >= 1.0)
                ++s.clipped;
        }

        s.peak = std::max (s.peak, framePeak);

        if (framePeak < Impl::silenceThreshold)
        {
            if (s.silenceStart < 0)
                s.silenceStart = s.frames + i;
        }
        else
        {
            s.closeSilence (s.frames + i);
        }
    }

    s.frames += numFrames;
    s.framesSinceShortTerm += numFrames;

    if ((double) s.framesSinceShortTerm >= s.sampleRate * 0.5)
    {
        s.framesSinceShortTerm = 0;
        double st = -HUGE_VAL;

        if (ebur128_loudness_shortterm (s.state, &st) == EBUR128_SUCCESS && std::isfinite (st))
            s.shortTermMax = std::max (s.shortTermMax, st);
    }
}

AudioStats StatsAccumulator::finish()
{
    auto& s = *impl_;
    s.closeSilence (s.frames);

    AudioStats out;
    out.durationS = (double) s.frames / s.sampleRate;

    double v = -HUGE_VAL;

    if (ebur128_loudness_global (s.state, &v) == EBUR128_SUCCESS)
        out.integratedLufs = finiteOrSilence (v);

    if (ebur128_loudness_shortterm (s.state, &v) == EBUR128_SUCCESS && std::isfinite (v))
        s.shortTermMax = std::max (s.shortTermMax, v);

    out.shortTermMaxLufs = finiteOrSilence (s.shortTermMax);

    double tp = 0;

    for (int c = 0; c < s.channels; ++c)
    {
        double p = 0;

        if (ebur128_true_peak (s.state, (unsigned) c, &p) == EBUR128_SUCCESS)
            tp = std::max (tp, p);
    }

    out.truePeakDbtp = linearToDb (std::max (tp, s.peak));
    out.samplePeakDbfs = linearToDb (s.peak);
    out.rmsDbfs = s.frames > 0 ? finiteOrSilence (10.0 * std::log10 (std::max (s.sumSquares / ((double) s.frames * s.channels), 1.0e-20))) : silenceDb;
    out.clippedSamples = s.clipped;
    out.silenceRanges = s.silence;
    return out;
}

AudioStats analyse (const std::vector<std::vector<float>>& planar, double sampleRate)
{
    StatsAccumulator acc ((int) planar.size(), sampleRate);
    std::vector<const float*> ptrs;

    for (auto& c : planar)
        ptrs.push_back (c.data());

    if (! planar.empty())
        acc.add (ptrs.data(), (int) planar.front().size());

    return acc.finish();
}

} // namespace rovox
