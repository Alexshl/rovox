// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Rovox stock effects (EQ, Comp, Gate, Limit, Delay, Verb): the DSP against its design (RBJ responses, static compressor curve, gate
// state machine, true-peak ceiling, exact delay taps, reverb tail), safety (silence, NaN, denormals, determinism, block-size independence)
// and the integration: plugin.catalog, fx.add / fx.params / fx.update with units, undo, deterministic offline renders.
#include "Limiter.h"
#include "Render.h"
#include "StockFx.h"
#include "StockPlugins.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <set>

using namespace rovox;

namespace
{
using Planar = std::vector<std::vector<float>>;
constexpr double pi = 3.14159265358979323846;

std::vector<float> sineWave (double hz, double amp, size_t n, double fs)
{
    std::vector<float> s (n);

    for (size_t i = 0; i < n; ++i)
        s[i] = (float) (amp * std::sin (2.0 * pi * hz * (double) i / fs));

    return s;
}

double rmsDb (const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;

    for (size_t i = from; i < to; ++i)
        sum += (double) x[i] * x[i];

    return 10.0 * std::log10 (std::max (sum / (double) (to - from), 1.0e-30));
}

/** Runs a processor over planar audio in blocks of `block` samples. */
template <typename P>
Planar run (P& p, Planar in, int block)
{
    const size_t n = in[0].size();
    std::vector<float*> ptr (in.size());

    for (size_t pos = 0; pos < n; pos += (size_t) block)
    {
        const int count = (int) std::min ((size_t) block, n - pos);

        for (size_t c = 0; c < in.size(); ++c)
            ptr[c] = in[c].data() + pos;

        p.process (ptr.data(), (int) in.size(), count);
    }

    return in;
}

/** Peak of the signal between the samples too (8x windowed-sinc interpolation, independent of the limiter's own detector). */
double truePeakDb (const std::vector<float>& x)
{
    double best = 0.0;

    for (size_t i = 0; i < x.size(); ++i)
        best = std::max (best, (double) std::abs (x[i]));

    for (int phase = 1; phase < 8; ++phase)
    {
        const double t = phase / 8.0;
        double h[64];

        for (int k = 0; k < 64; ++k)
        {
            const double d = (k - 31) - t;
            const double sinc = std::abs (d) < 1e-12 ? 1.0 : std::sin (pi * d) / (pi * d);
            h[k] = sinc * (0.5 + 0.5 * std::cos (pi * d / 32.5));
        }

        for (size_t i = 40; i + 40 < x.size(); ++i)
        {
            double acc = 0.0;

            for (int k = 0; k < 64; ++k)
                acc += h[k] * x[i + (size_t) k - 31];

            best = std::max (best, std::abs (acc));
        }
    }

    return 20.0 * std::log10 (std::max (best, 1e-12));
}

Planar noiseAndBursts (size_t n, double fs, unsigned seed = 1)
{
    Planar x (2, std::vector<float> (n));
    unsigned s = seed;

    for (size_t i = 0; i < n; ++i)
    {
        for (size_t c = 0; c < 2; ++c)
        {
            s = s * 1664525u + 1013904223u;
            const double noise = ((double) (s >> 8) / (double) (1u << 24) - 0.5) * 0.4;
            const bool burst = (i / (size_t) (fs * 0.25)) % 3 == 0;
            x[c][i] = (float) (noise + (burst ? 0.8 * std::sin (2.0 * pi * 220.0 * (double) i / fs + (double) c) : 0.0));
        }
    }

    return x;
}

/** Peaky music-like material: a few partials, a decaying hit every 0.4 s and a little low-passed noise, so that inter-sample peaks exist. */
Planar peaky (size_t n, double fs)
{
    Planar x (2, std::vector<float> (n));
    unsigned s = 5;
    double lp[2] = { 0.0, 0.0 };

    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        const double hit = std::exp (-std::fmod (t, 0.4) * 30.0);

        for (size_t c = 0; c < 2; ++c)
        {
            s = s * 1664525u + 1013904223u;
            lp[c] += 0.1 * (((double) (s >> 8) / (double) (1u << 24) - 0.5) - lp[c]);
            const double ph = (double) c * 0.7;
            x[c][i] = (float) (0.35 * std::sin (2.0 * pi * 196.0 * t + ph) + 0.25 * std::sin (2.0 * pi * 1320.0 * t + 2.0 * ph)
                               + 0.45 * hit * std::sin (2.0 * pi * 5100.0 * t + ph) + 0.4 * hit * std::sin (2.0 * pi * 9300.0 * t) + 0.3 * lp[c]);
        }
    }

    return x;
}

bool sameBits (const Planar& a, const Planar& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t c = 0; c < a.size(); ++c)
        if (a[c].size() != b[c].size() || std::memcmp (a[c].data(), b[c].data(), a[c].size() * sizeof (float)) != 0)
            return false;

    return true;
}

bool finiteAndNormal (const Planar& x)
{
    for (auto& ch : x)
        for (float v : ch)
            if (! std::isfinite (v) || std::fpclassify (v) == FP_SUBNORMAL)
                return false;

    return true;
}

/** A DSP object with its input-independent setup, so that the safety tests can treat all six alike. */
struct Dsp
{
    std::string name;
    std::function<Planar (Planar, int)> process;     // fresh instance per call
    int latency = 0;
};

fx::EqParams demoEq()
{
    fx::EqParams p;
    p.bands[0] = { true, fx::BandType::LowShelf, 120.0, 5.0, 2.0 };
    p.bands[1] = { true, fx::BandType::Peak, 400.0, -4.0, 1.0 };
    p.bands[2] = { true, fx::BandType::Peak, 3000.0, 6.0, 0.7 };
    p.bands[3] = { true, fx::BandType::HighShelf, 9000.0, -3.0, 2.0 };
    p.bands[4] = { true, fx::BandType::HighPass, 40.0, 0.0, 1.0 };
    p.outputGainDb = 1.5;
    return p;
}

std::vector<Dsp> allDsp (double fs)
{
    std::vector<Dsp> out;
    out.push_back ({ "eq", [fs] (Planar in, int block) { fx::Eq e; e.prepare (fs); e.setParams (demoEq()); return run (e, std::move (in), block); }, 0 });
    out.push_back ({ "comp", [fs] (Planar in, int block)
    {
        fx::Comp c; c.prepare (fs);
        fx::CompParams p; p.thresholdDb = -24; p.ratio = 6; p.lookaheadMs = 3; p.rmsMs = 10; p.autoMakeup = true;
        c.setParams (p);
        return run (c, std::move (in), block);
    }, 0 });
    out.push_back ({ "gate", [fs] (Planar in, int block) { fx::Gate g; g.prepare (fs); fx::GateParams p; p.thresholdDb = -30; g.setParams (p); return run (g, std::move (in), block); }, 0 });
    out.push_back ({ "limit", [fs] (Planar in, int block)
    {
        StreamingLimiter l; l.prepare (fs, 2); l.setParams (6.0, -1.0, 80.0);
        return run (l, std::move (in), block);
    }, 0 });
    out.push_back ({ "delay", [fs] (Planar in, int block)
    {
        fx::Delay d; d.prepare (fs);
        fx::DelayParams p; p.feedbackPct = 60; p.lowPassHz = 5000; p.highPassHz = 100;
        p.taps[1] = { true, false, 333.0, 0.5, -3.0, -0.5 };
        p.taps[2] = { true, true, 250.0, 0.75, -6.0, 0.5 };
        d.setParams (p);
        return run (d, std::move (in), block);
    }, 0 });
    out.push_back ({ "verb", [fs] (Planar in, int block)
    {
        fx::Verb v; v.prepare (fs);
        fx::VerbParams p; p.roomSize = 0.9; p.preDelayMs = 20; p.lowCutHz = 100; p.highCutHz = 8000;
        v.setParams (p);
        return run (v, std::move (in), block);
    }, 0 });
    return out;
}

//==============================================================================
std::string sineTrackProject (test::Env& env, double seconds = 3.0)
{
    const auto id = env.newProject ("stock");
    const auto dir = test::makeTempDir ("stock-sine");
    test::writeWav (dir / "sine.wav", { test::sine (330.0, -18.0, seconds, 44100.0) }, 44100.0, 24);
    auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "sine.wav").string() } } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    return id;
}

json findParam (const json& fxInfo, const std::string& ident)
{
    for (auto& p : fxInfo["params"])
        if (p["ident"] == ident)
            return p;

    return nullptr;
}
} // namespace

//==============================================================================
ROVOX_TEST (stock, eq_response_at_band_centres_matches_the_design)
{
    const double fs = 48000.0;

    struct Case { fx::BandType type; double f0, gain, bw; };
    const Case cases[] = { { fx::BandType::Peak, 1000, 6, 1 }, { fx::BandType::Peak, 3000, -9, 0.5 }, { fx::BandType::Peak, 100, 12, 2 },
                           { fx::BandType::LowShelf, 120, 6, 2 }, { fx::BandType::LowShelf, 250, -8, 1.5 },
                           { fx::BandType::HighShelf, 8000, 6, 2 }, { fx::BandType::HighShelf, 5000, -10, 1 },
                           { fx::BandType::LowPass, 2000, 0, 1 }, { fx::BandType::HighPass, 300, 0, 1.9 },
                           { fx::BandType::BandPass, 1500, 0, 1 }, { fx::BandType::Notch, 4000, 0, 1 } };

    for (const auto& c : cases)
    {
        fx::Eq eq;
        eq.prepare (fs);
        fx::EqParams p;
        p.bands[0] = { true, c.type, c.f0, c.gain, c.bw };
        eq.setParams (p);

        const size_t n = (size_t) fs;
        auto out = run (eq, { sineWave (c.f0, 0.25, n, fs) }, 480);
        const double measured = rmsDb (out[0], n / 2, n) - rmsDb (sineWave (c.f0, 0.25, n, fs), n / 2, n);
        const double q = std::sqrt (std::pow (2.0, c.bw)) / (std::pow (2.0, c.bw) - 1.0);
        double expected = 0.0;

        switch (c.type)
        {
            case fx::BandType::Peak:        expected = c.gain; break;
            case fx::BandType::LowShelf:
            case fx::BandType::HighShelf:   expected = c.gain / 2.0; break;
            case fx::BandType::LowPass:
            case fx::BandType::HighPass:    expected = 20.0 * std::log10 (q); break;
            case fx::BandType::BandPass:    expected = 0.0; break;
            case fx::BandType::Notch:       expected = -200.0; break;
        }

        if (c.type == fx::BandType::Notch)
            CHECK (measured < -40.0);
        else
            CHECK_NEAR (measured, expected, 0.2);

        CHECK_NEAR (eq.magnitudeDb (c.f0), c.type == fx::BandType::Notch ? eq.magnitudeDb (c.f0) : expected, 0.01);
    }

    // a chain of bands: the measured response follows magnitudeDb() everywhere
    fx::Eq eq;
    eq.prepare (fs);
    eq.setParams (demoEq());

    for (double f : { 60.0, 200.0, 400.0, 1000.0, 3000.0, 7000.0, 12000.0 })
    {
        fx::Eq e;
        e.prepare (fs);
        e.setParams (demoEq());
        const size_t n = (size_t) fs;
        auto out = run (e, { sineWave (f, 0.1, n, fs) }, 256);
        const double measured = rmsDb (out[0], n / 2, n) - rmsDb (sineWave (f, 0.1, n, fs), n / 2, n);
        CHECK_NEAR (measured, eq.magnitudeDb (f) + 1.5, 0.1);
    }

    // a disabled band does nothing
    fx::EqParams off;
    off.bands[0] = { false, fx::BandType::Peak, 1000, 12, 1 };
    fx::Eq e;
    e.prepare (fs);
    e.setParams (off);
    CHECK_NEAR (e.magnitudeDb (1000), 0.0, 1e-9);
}

ROVOX_TEST (stock, comp_static_curve_follows_the_ratio_above_the_threshold)
{
    const double fs = 48000.0;
    fx::CompParams p;
    p.thresholdDb = -20.0;
    p.ratio = 4.0;
    p.attackMs = 1.0;
    p.releaseMs = 50.0;
    p.kneeDb = 0.0;
    p.rmsMs = 20.0;

    for (double inRmsDb : { -40.0, -30.0, -20.0, -14.0, -10.0, -4.0 })
    {
        fx::Comp c;
        c.prepare (fs);
        c.setParams (p);
        const size_t n = (size_t) (fs * 1.5);
        const double amp = std::pow (10.0, (inRmsDb + 3.0103) / 20.0);
        auto out = run (c, { sineWave (1000.0, amp, n, fs) }, 512);
        const double outRms = rmsDb (out[0], n * 2 / 3, n);
        const double expected = inRmsDb > p.thresholdDb ? p.thresholdDb + (inRmsDb - p.thresholdDb) / p.ratio : inRmsDb;
        CHECK_NEAR (outRms, expected, 0.3);

        if (inRmsDb > p.thresholdDb)
            CHECK_NEAR (c.lastGainReductionDb(), inRmsDb - expected, 0.3);
    }

    // soft knee: continuous curve, half the slope change at the threshold, no reduction below the knee
    p.kneeDb = 10.0;
    CHECK_NEAR (fx::Comp::gainReductionDb (p, -26.0), 0.0, 1e-9);
    CHECK_NEAR (fx::Comp::gainReductionDb (p, -20.0), 0.9375, 1e-9);
    CHECK_NEAR (fx::Comp::gainReductionDb (p, -15.0 + 1e-9), fx::Comp::gainReductionDb (p, -15.0 - 1e-9), 1e-6);
    CHECK_NEAR (fx::Comp::gainReductionDb (p, -5.0), 0.75 * 15.0, 1e-9);

    // makeup and the wet mix
    p.kneeDb = 0.0;
    p.makeupDb = 6.0;
    fx::Comp c;
    c.prepare (fs);
    c.setParams (p);
    auto quiet = run (c, { sineWave (1000.0, 0.01, 4800, fs) }, 512);
    CHECK_NEAR (rmsDb (quiet[0], 2400, 4800) - rmsDb (sineWave (1000.0, 0.01, 4800, fs), 2400, 4800), 6.0, 0.05);

    // lookahead delays the audio by exactly that many samples and reports it
    fx::CompParams look;
    look.lookaheadMs = 5.0;
    fx::Comp lc;
    lc.prepare (fs);
    lc.setParams (look);
    CHECK (lc.latencySamples() == 240);
    Planar impulse { std::vector<float> (2000, 0.0f) };
    impulse[0][10] = 0.001f;
    auto delayed = run (lc, impulse, 100);
    CHECK_NEAR (delayed[0][250], 0.001, 1e-7);
    CHECK_NEAR (delayed[0][10], 0.0, 1e-9);
}

ROVOX_TEST (stock, gate_closes_below_the_threshold_and_has_hysteresis)
{
    const double fs = 48000.0;
    fx::GateParams p;
    p.thresholdDb = -40.0;
    p.attackMs = 1.0;
    p.holdMs = 20.0;
    p.releaseMs = 20.0;
    p.hysteresisDb = 3.0;
    p.rangeDb = -60.0;

    auto amp = [] (double db) { return std::pow (10.0, db / 20.0); };

    // loud passes, quiet is cut by the range
    {
        fx::Gate g;
        g.prepare (fs);
        g.setParams (p);
        auto loud = run (g, { sineWave (500.0, amp (-20.0), 24000, fs) }, 256);
        CHECK (g.isOpen());
        CHECK_NEAR (rmsDb (loud[0], 12000, 24000) - rmsDb (sineWave (500.0, amp (-20.0), 24000, fs), 12000, 24000), 0.0, 0.1);

        auto quiet = run (g, { sineWave (500.0, amp (-50.0), 48000, fs) }, 256);
        CHECK (! g.isOpen());
        const double atten = rmsDb (quiet[0], 36000, 48000) - rmsDb (sineWave (500.0, amp (-50.0), 48000, fs), 36000, 48000);
        CHECK_NEAR (atten, -60.0, 1.0);
    }

    // below the threshold from the start: closed
    {
        fx::Gate g;
        g.prepare (fs);
        g.setParams (p);
        auto quiet = run (g, { sineWave (500.0, amp (-45.0), 24000, fs) }, 256);
        CHECK (! g.isOpen());
        CHECK (rmsDb (quiet[0], 12000, 24000) < -45.0 - 3.0 - 50.0);
    }

    // hysteresis: -41.5 dB peak is under the threshold but inside the 3 dB band: an open gate stays open, a closed one stays closed
    {
        fx::Gate g;
        g.prepare (fs);
        g.setParams (p);
        run (g, { sineWave (500.0, amp (-20.0), 12000, fs) }, 256);
        run (g, { sineWave (500.0, amp (-41.5), 48000, fs) }, 256);
        CHECK (g.isOpen());

        fx::Gate h;
        h.prepare (fs);
        h.setParams (p);
        run (h, { sineWave (500.0, amp (-41.5), 48000, fs) }, 256);
        CHECK (! h.isOpen());
    }

    // hold keeps the gate open for the hold time after the signal drops below threshold - hysteresis
    {
        fx::GateParams hold = p;
        hold.holdMs = 100.0;
        fx::Gate g;
        g.prepare (fs);
        g.setParams (hold);
        run (g, { sineWave (500.0, amp (-20.0), 12000, fs) }, 256);
        run (g, { std::vector<float> (2400, 0.0f) }, 256);       // 50 ms of silence
        CHECK (g.isOpen());
        run (g, { std::vector<float> (9600, 0.0f) }, 256);       // 250 ms in total
        CHECK (! g.isOpen());
    }
}

ROVOX_TEST (stock, limiter_keeps_the_true_peak_under_the_ceiling_in_a_stream)
{
    const double fs = 48000.0;
    auto input = peaky ((size_t) (fs * 3), fs);

    for (int block : { 64, 480, 1000 })
    {
        StreamingLimiter l;
        l.prepare (fs, 2);
        l.setParams (9.0, -1.0, 80.0, 0.1);
        auto out = run (l, input, block);
        double worst = -200.0;

        for (auto& ch : out)
            worst = std::max (worst, truePeakDb (ch));

        std::cerr << "    block " << block << ": true peak " << worst << " dBTP, max reduction " << l.lastGainReductionDb() << " dB\n";
        CHECK (worst <= -1.0);
        CHECK (worst > -3.0);           // it limits, it does not squash
    }

    // the stream gives the same samples whatever the block size, and the offline limiter agrees away from the end
    StreamingLimiter a, b;
    a.prepare (fs, 2);
    b.prepare (fs, 2);
    a.setParams (6.0, -1.0, 80.0);
    b.setParams (6.0, -1.0, 80.0);
    auto small = run (a, input, 37);
    auto large = run (b, input, 4096);
    CHECK (sameBits (small, large));

    Planar offline = input;
    limitTruePeak (offline, fs, 6.0);
    const int lat = a.latencySamples();
    double maxDiff = 0.0;

    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i + (size_t) lat + 2000 < input[0].size(); ++i)
            maxDiff = std::max (maxDiff, (double) std::abs (small[c][i + (size_t) lat] - offline[c][i]));

    CHECK (maxDiff < 1e-5);
    CHECK (lat == (int) std::llround (0.0015 * fs) + 8);
}

ROVOX_TEST (stock, delay_tap_lands_on_the_exact_sample)
{
    const double fs = 48000.0;
    fx::DelayParams p;
    p.taps[0] = { true, false, 10.0, 0.5, 0.0, 0.0 };
    p.taps[1] = { false, false, 20.0, 0.5, 0.0, 0.0 };
    p.taps[2] = { false, false, 30.0, 0.5, 0.0, 0.0 };
    p.taps[3] = { false, false, 40.0, 0.5, 0.0, 0.0 };
    p.feedbackPct = 0.0;
    p.wetDb = 0.0;
    p.dryDb = -96.0;

    Planar x (2, std::vector<float> (48000, 0.0f));
    x[0][100] = 1.0f;
    x[1][333] = -0.5f;

    for (int block : { 64, 480, 777 })
    {
        fx::Delay d;
        d.prepare (fs);
        d.setParams (p);
        auto out = run (d, x, block);
        CHECK (out[0][580] == 1.0f);
        CHECK (out[1][813] == -0.5f);
        double others = 0.0;

        for (size_t i = 0; i < out[0].size(); ++i)
        {
            if (i != 580)  others = std::max (others, (double) std::abs (out[0][i]));
            if (i != 813)  others = std::max (others, (double) std::abs (out[1][i]));
        }

        CHECK (others == 0.0);
    }

    // tempo sync: half a beat at 120 bpm = 250 ms = 12000 samples; a second tap with gain and pan
    p.taps[0] = { true, true, 10.0, 0.5, 0.0, 0.0 };
    p.taps[1] = { true, false, 5.0, 0.5, -6.0206, -1.0 };       // 240 samples, all left
    p.bpm = 120.0;
    fx::Delay d;
    d.prepare (fs);
    d.setParams (p);
    auto out = run (d, x, 500);
    CHECK (out[0][100 + 12000] == 1.0f);
    CHECK_NEAR (out[0][100 + 240], 0.5, 1e-4);
    CHECK (out[1][100 + 240] == 0.0f);

    // feedback: echoes every 10 ms at half the level
    fx::DelayParams fb;
    fb.taps[0] = { true, false, 10.0, 0.5, 0.0, 0.0 };
    fb.taps[1].enabled = fb.taps[2].enabled = fb.taps[3].enabled = false;
    fb.feedbackPct = 50.0;
    fb.wetDb = 0.0;
    fb.dryDb = -96.0;
    fx::Delay f;
    f.prepare (fs);
    f.setParams (fb);
    auto echoes = run (f, x, 300);
    CHECK_NEAR (echoes[0][580], 1.0, 1e-6);
    CHECK_NEAR (echoes[0][1060], 0.5, 1e-6);
    CHECK_NEAR (echoes[0][1540], 0.25, 1e-6);
    CHECK (fx::Delay::tailSeconds (fb) > 0.1);
}

ROVOX_TEST (stock, reverb_has_a_tail_that_decays)
{
    const double fs = 44100.0;
    fx::VerbParams p;
    p.roomSize = 0.7;
    p.dryDb = -96.0;
    p.wetDb = 0.0;

    Planar x (2, std::vector<float> ((size_t) (fs * 6), 0.0f));
    x[0][0] = 1.0f;
    fx::Verb v;
    v.prepare (fs);
    v.setParams (p);
    auto out = run (v, x, 512);

    auto energy = [&] (double from, double to)
    {
        double e = 0.0;

        for (size_t c = 0; c < 2; ++c)
            for (size_t i = (size_t) (from * fs); i < (size_t) (to * fs); ++i)
                e += (double) out[c][i] * out[c][i];

        return e;
    };

    const double early = energy(0.05, 0.25), mid = energy (0.5, 0.7), late = energy (2.0, 2.2), end = energy (5.5, 5.7);
    std::cerr << "    tail energy early " << early << " mid " << mid << " late " << late << " end " << end << "\n";
    CHECK (early > 1e-6);
    CHECK (mid > 1e-9);
    CHECK (mid < early);
    CHECK (late < mid);
    CHECK (end < late);
    CHECK (end < early * 1e-3);

    // a bigger room rings longer, more damping darkens it; the wet signal is stereo (decorrelated channels)
    fx::VerbParams big = p;
    big.roomSize = 0.95;
    fx::Verb vb;
    vb.prepare (fs);
    vb.setParams (big);
    auto bigOut = run (vb, x, 512);
    double eBig = 0.0, eSmall = 0.0;

    for (size_t i = (size_t) (2.0 * fs); i < (size_t) (2.2 * fs); ++i)
    {
        eBig += (double) bigOut[0][i] * bigOut[0][i];
        eSmall += (double) out[0][i] * out[0][i];
    }

    CHECK (eBig > eSmall);
    double diff = 0.0;

    for (size_t i = 0; i < 20000; ++i)
        diff += std::abs (out[0][i] - out[1][i]);

    CHECK (diff > 1e-3);
    CHECK (fx::Verb::tailSeconds (big) > fx::Verb::tailSeconds (p));
}

ROVOX_TEST (stock, all_effects_are_silent_finite_without_denormals_and_deterministic)
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        for (auto& d : allDsp (fs))
        {
            // silence in, silence out
            Planar silence (2, std::vector<float> ((size_t) fs, 0.0f));
            auto quiet = d.process (silence, 480);

            for (auto& ch : quiet)
                for (float v : ch)
                    CHECK (v == 0.0f);

            // loud, noisy, bursty input followed by a long silence: finite, no subnormals, and a decay to (almost) nothing
            auto sig = noiseAndBursts ((size_t) (fs * 2), fs);

            for (auto& ch : sig)
                ch.resize ((size_t) (fs * 12), 0.0f);

            auto a = d.process (sig, 480);
            auto b = d.process (sig, 480);
            const bool ok = finiteAndNormal (a);
            CHECK (ok);

            if (! ok)
                std::cerr << "    not finite / subnormal: " << d.name << " at " << fs << "\n";

            CHECK (sameBits (a, b));                                 // deterministic
            const bool blockIndependent = sameBits (a, d.process (sig, 61));
            CHECK (blockIndependent);                                // and independent of the block size

            if (! blockIndependent)
                std::cerr << "    block size changes the output of " << d.name << " at " << fs << "\n";

            // DC and full-scale square waves do not blow up
            Planar harsh (2, std::vector<float> ((size_t) fs, 1.0f));

            for (size_t i = 0; i < harsh[0].size(); i += 100)
                for (size_t k = 0; k < 50 && i + k < harsh[0].size(); ++k)
                    harsh[0][i + k] = harsh[1][i + k] = -1.0f;

            auto h = d.process (harsh, 333);

            for (auto& ch : h)
                for (float v : ch)
                    CHECK (std::isfinite (v) && std::abs (v) < 100.0f);
        }
    }
}

//==============================================================================
ROVOX_TEST (stock, catalog_lists_the_stock_effects_and_fx_add_takes_them_by_id_or_name)
{
    test::Env env;
    auto cat = env.ok ("", "plugin.catalog", { { "query", "stock" }, { "limit", 50 } });
    CHECK (cat["total"].get<int>() >= 6);
    std::set<std::string> uids;

    for (auto& p : cat["plugins"])
    {
        CHECK (p["format"] == "Rovox");
        CHECK (std::find (p["categories"].begin(), p["categories"].end(), "stock") != p["categories"].end());
        CHECK (p["status"] == "ok");
        CHECK (p["kind"] == "effect");
        uids.insert (p["plugin_uid"].get<std::string>());
    }

    for (const char* uid : { "rovox:eq", "rovox:comp", "rovox:gate", "rovox:limit", "rovox:delay", "rovox:verb" })
        CHECK (uids.count (uid) == 1);

    // the only Rovox instrument is the drum sampler
    auto instruments = env.ok ("", "plugin.catalog", { { "kind", "instrument" }, { "query", "Rovox" } })["plugins"];
    REQUIRE (instruments.size() == 1);
    CHECK (instruments[0]["plugin_uid"] == "rovox:drums");
    CHECK (instruments[0]["io"]["midi_in"] == true);
    CHECK (env.ok ("", "plugin.probe", { { "plugin_uid", "rovox:verb" } })["probe"]["builtin"] == true);

    const auto id = sineTrackProject (env);
    const auto track = env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();
    const auto before = test::snapshot (env, id);

    auto eq = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "rovox:eq" } } } });
    auto comp = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "name", "Rovox Comp" } } } });
    auto gate = env.ok (id, "fx.add", { { "track_id", track }, { "builtin", "rovox_gate" } });
    CHECK (eq["plugin"]["name"] == "Rovox EQ");
    CHECK (comp["plugin"]["name"] == "Rovox Comp");
    CHECK (gate["plugin"]["name"] == "Rovox Gate");
    CHECK (! env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "name", "Rovox Nope" } } } }).ok);

    // before the fader, in the order added
    auto list = env.ok (id, "fx.list", { { "target", track } })["fx"];
    REQUIRE (list.size() >= 4);
    CHECK (list[0]["name"] == "Rovox EQ");
    CHECK (list[1]["name"] == "Rovox Comp");
    CHECK (list[2]["name"] == "Rovox Gate");
    CHECK (list[3]["is_fader"] == true);

    // params with units; defaults are the documented ones
    const auto eqId = eq["plugin_id"].get<std::string>();
    auto info = env.ok (id, "fx.params", { { "plugin_id", eqId } })["fx"];
    CHECK (info["stock"] == true);
    CHECK (info["units"]["b2_freq"] == "Hz");
    CHECK (info["units"]["b2_gain"] == "dB");
    CHECK (info["units"]["b2_bw"] == "oct");
    CHECK (info["params"].size() == 41);
    CHECK_NEAR (findParam (info, "b2_freq")["value"].get<double>(), 400.0, 1e-3);
    CHECK (findParam (info, "b1_type")["display"] == "low_shelf");
    CHECK (findParam (info, "b4_type")["display"] == "high_shelf");
    CHECK (findParam (info, "b5_on")["display"] == "off");

    // fx.update: numbers in the unit, text with the unit, choice names; one undo step per call
    const auto stepsBefore = env.ok (id, "history.list")["undo"].size();
    env.ok (id, "fx.update", { { "plugin_id", eqId }, { "params", { { "b2_gain", -4.5 }, { "b2_freq", "650 Hz" }, { "b2_type", "notch" }, { "b5_on", "on" } } } });
    CHECK (env.ok (id, "history.list")["undo"].size() == stepsBefore + 1);
    info = env.ok (id, "fx.params", { { "plugin_id", eqId } })["fx"];
    CHECK_NEAR (findParam (info, "b2_gain")["value"].get<double>(), -4.5, 1e-4);
    CHECK_NEAR (findParam (info, "b2_freq")["value"].get<double>(), 650.0, 1e-3);
    CHECK (findParam (info, "b2_type")["display"] == "notch");
    CHECK (findParam (info, "b5_on")["value"].get<double>() == 1.0);
    CHECK (! env.run (id, "fx.update", { { "plugin_id", eqId }, { "params", { { "b2_gain", 99.0 } } } }).ok);        // out of range

    env.ok (id, "undo");
    info = env.ok (id, "fx.params", { { "plugin_id", eqId } })["fx"];
    CHECK_NEAR (findParam (info, "b2_gain")["value"].get<double>(), 0.0, 1e-4);
    CHECK_NEAR (findParam (info, "b2_freq")["value"].get<double>(), 400.0, 1e-3);
    CHECK (findParam (info, "b2_type")["display"] == "peak");
    env.ok (id, "redo");
    CHECK_NEAR (findParam (env.ok (id, "fx.params", { { "plugin_id", eqId } })["fx"], "b2_gain")["value"].get<double>(), -4.5, 1e-4);
    env.ok (id, "undo");

    // comp: meter and units
    auto compInfo = env.ok (id, "fx.params", { { "plugin_id", comp["plugin_id"] } })["fx"];
    CHECK (compInfo["meters"].contains ("gain_reduction_db"));
    CHECK (compInfo["units"]["threshold"] == "dB");
    CHECK (compInfo["units"]["attack"] == "ms");
    env.ok (id, "fx.update", { { "plugin_id", comp["plugin_id"] }, { "params", { { "threshold", "-30 dB" }, { "ratio", 8 }, { "lookahead", 5 } } } });
    CHECK (env.ok (id, "fx.params", { { "plugin_id", comp["plugin_id"] } })["fx"]["latency_samples"].get<int>() > 0);

    // undo of every fx.add brings back the original chain
    env.ok (id, "undo");
    env.ok (id, "undo");
    env.ok (id, "undo");
    env.ok (id, "undo");
    CHECK (test::snapshot (env, id) == before);
    env.ok (id, "redo");
    CHECK (env.ok (id, "fx.list", { { "target", track } })["fx"].size() >= 2);
}

ROVOX_TEST (stock, an_edit_with_all_six_renders_the_same_twice_and_the_limiter_reports_latency)
{
    test::Env env;
    const auto id = sineTrackProject (env, 2.0);
    const auto track = env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();
    std::string limitId;

    for (const char* uid : { "rovox:eq", "rovox:comp", "rovox:gate", "rovox:delay", "rovox:verb", "rovox:limit" })
    {
        auto added = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", uid } } } });

        if (std::string (uid) == "rovox:limit")
            limitId = added["plugin_id"].get<std::string>();
    }

    auto info = env.ok (id, "fx.params", { { "plugin_id", limitId } })["fx"];
    CHECK (info["latency_samples"].get<int>() >= 8);
    CHECK (info["meters"].contains ("gain_reduction_db"));

    auto render = [&] (const char* name)
    {
        RenderRequest req;
        req.name = name;
        req.tailS = 1.0;
        auto r = renderProject (env.project (id), req);
        REQUIRE (r.ok());
        Planar ch;
        double sr = 0;
        REQUIRE (test::readWav (env.project (id).rendersDir() / (std::string (name) + ".wav"), ch, sr));
        return ch;
    };

    const auto a = render ("one"), b = render ("two");
    CHECK (sameBits (a, b));
    double peak = 0.0;

    for (auto& ch : a)
        for (float v : ch)
        {
            CHECK (std::isfinite (v));
            peak = std::max (peak, (double) std::abs (v));
        }

    CHECK (peak > 0.01);
    CHECK (peak < 1.0);
}
