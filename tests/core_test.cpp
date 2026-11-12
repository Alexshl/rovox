// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Cgroup.h"
#include "Ids.h"
#include "Spectrum.h"
#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

using namespace rovox;

namespace
{
/** Deterministic pink noise at the given RMS: a fixed-seed xorshift white source through Paul Kellet's refined filter (+-0.05 dB from 9.2 Hz up at 44.1 kHz). */
std::vector<float> pinkNoise (double seconds, double sampleRate, double rmsDbfs)
{
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    auto white = [&]
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return (double) (state >> 11) / (double) (1ull << 52) - 1.0;        // [-1, 1)
    };

    const size_t n = (size_t) (seconds * sampleRate);
    std::vector<double> x (n);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum2 = 0;

    for (auto& v : x)
    {
        const double w = white();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        sum2 += v * v;
    }

    const double gain = std::pow (10.0, rmsDbfs / 20.0) / std::sqrt (sum2 / (double) n);
    std::vector<float> out (n);

    for (size_t i = 0; i < n; ++i)
        out[i] = (float) (x[i] * gain);

    return out;
}

/** Pink noise has power ~ 1/f, so a band holds power proportional to its width in octaves. Band offsets relative to lowmid_250_2k, in dB, from the band edges of Spectrum.cpp. */
double pinkOffsetDb (double lo, double hi)
{
    return 10.0 * std::log10 (std::log2 (hi / lo) / std::log2 (2000.0 / 250.0));
}
}

ROVOX_TEST (core, ulid_sorted_and_unique)
{
    std::set<std::string> seen;
    std::string previous;

    for (int i = 0; i < 20000; ++i)
    {
        auto id = newId ("trk");
        CHECK (isValidId (id, "trk"));
        CHECK (id > previous);
        previous = id;
        seen.insert (id);
    }

    CHECK (seen.size() == 20000);
    CHECK (! isValidId ("trk_short", "trk"));
    CHECK (! isValidId (newId ("itm"), "trk"));
}

ROVOX_TEST (core, tenant_ids)
{
    CHECK (isValidTenantId ("local"));
    CHECK (isValidTenantId ("a_b-9"));
    CHECK (! isValidTenantId ("../x"));
    CHECK (! isValidTenantId ("A"));
    CHECK (! isValidTenantId (""));
    CHECK (! isValidTenantId (std::string (65, 'a')));
}

ROVOX_TEST (core, cgroup_cpu_max)
{
    CHECK_NEAR (parseCpuMax ("max 100000"), 0.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("200000 100000"), 2.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("150000 100000\n"), 1.5, 1e-9);
    CHECK_NEAR (parseCpuMax (""), 0.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("garbage"), 0.0, 1e-9);
    CHECK (cpuBudget().cpus >= 1);
}

ROVOX_TEST (core, spectrum_of_pink_noise_follows_band_width_in_octaves)
{
    // 1/f noise: equal power per octave, so each band sits at 10*log10(octaves of the band / octaves of lowmid) relative to lowmid (not monotonically)
    constexpr double sr = 44100.0;
    const auto noise = pinkNoise (30.0, sr, -20.0);
    SpectrumAnalyser analyser (sr);
    analyser.add (noise.data(), (int) noise.size());
    const auto bands = analyser.finish();

    CHECK_NEAR (bands.sub - bands.lowMid, pinkOffsetDb (20, 60), 1.0);
    CHECK_NEAR (bands.low - bands.lowMid, pinkOffsetDb (60, 250), 1.0);
    CHECK_NEAR (bands.highMid - bands.lowMid, pinkOffsetDb (2000, 6000), 1.0);
    CHECK_NEAR (bands.high - bands.lowMid, pinkOffsetDb (6000, 20000), 1.0);
}
