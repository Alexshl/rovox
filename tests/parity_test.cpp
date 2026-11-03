// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER parity of the item fades, the ReaEQ band types, ReaDelay, ReaGate, ReaComp and ReaLimit. The expectations are computed in this file from the formulas of the
// behaviour specification (fade curve warp and shapes, RBJ-style biquads with the ReaEQ bandwidth and shelf-slope rules, the ReaDelay tap
// loop); a handful of numbers from the measurements are kept as literals.
#include "Fades.h"
#include "ReaperFx.h"
#include "Render.h"
#include "RppImport.h"
#include "StockFx.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <cmath>
#include <fstream>

using namespace rovox;

namespace
{
constexpr double pi = 3.14159265358979323846;
using Planar = std::vector<std::vector<float>>;

fades::FadeCurve fc (int shape, double curve)       { return { shape, curve }; }

//------------------------------------------------------------------------------ fades (spec formulas)
double specWarp (double t, double c)
{
    const auto neg = [] (double x, double a)
    {
        if (a <= 0.5)
            return (1 - 2 * a) * x + 2 * a * x * x;

        const double b = 2 * a - 1;
        return (1 - b) * x * x + b * std::pow (x, 4.0);
    };

    return c <= 0 ? neg (t, -c) : 1 - neg (1 - t, c);
}

double specFade (int shape, double c, double t)
{
    const double u = specWarp (t, c);

    if (shape == 5)     return u < 0.5 ? 0.5 * std::pow (2 * u, 2.0) : 1 - 0.5 * std::pow (2 * (1 - u), 2.0);
    if (shape == 6)     return u < 0.5 ? 0.5 * std::pow (2 * u, 4.0) : 1 - 0.5 * std::pow (2 * (1 - u), 4.0);
    return u;
}

//------------------------------------------------------------------------------ ReaEQ (spec formulas)
struct Iir { double b[3], a[3]; };

Iir specBand (int code, double f0, double gainDb, double bw, double fs)
{
    const double w0 = 2 * pi * f0 / fs, c = std::cos (w0), s = std::sin (w0), A = std::pow (10.0, gainDb / 40.0);
    const double alpha = s * std::sinh (std::log (2.0) / 2 * bw * w0 / s);
    Iir r {};
    const auto set = [&] (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        r = { { b0 / a0, b1 / a0, b2 / a0 }, { 1, a1 / a0, a2 / a0 } };
    };

    switch (code)
    {
        case 8: case 9:     set (1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A); break;
        case 2:             set (1 + alpha * A * A, -2 * c, 1 - alpha * A * A, 1 + alpha, -2 * c, 1 - alpha); break;
        case 3:             set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha); break;
        case 4:             set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha); break;
        case 5:             set (1 - alpha, -2 * c, 1 + alpha, 1 + alpha, -2 * c, 1 - alpha); break;
        case 6:             set (1, -2 * c, 1, 1 + alpha, -2 * c, 1 - alpha); break;
        case 7: case 10:    set (alpha, 0, -alpha, 1 + alpha, -2 * c, 1 - alpha); break;
        default:
        {
            const double S = std::min (1 / (bw * bw), 1.2);
            const double al = s / 2 * std::sqrt ((A + 1 / A) * (1 / S - 1) + 2), beta = 2 * std::sqrt (A) * al;

            if (code == 0)
                set (A * ((A + 1) - (A - 1) * c + beta), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - beta),
                     (A + 1) + (A - 1) * c + beta, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - beta);
            else
                set (A * ((A + 1) + (A - 1) * c + beta), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - beta),
                     (A + 1) - (A - 1) * c + beta, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - beta);
        }
    }

    return r;
}

std::vector<double> iirImpulse (const Iir& f, size_t n)
{
    std::vector<double> y (n, 0.0);

    for (size_t i = 0; i < n; ++i)
    {
        double v = f.b[0] * (i == 0 ? 1.0 : 0.0) + (i >= 1 ? f.b[1] * (i == 1 ? 1.0 : 0.0) : 0.0) + (i >= 2 ? f.b[2] * (i == 2 ? 1.0 : 0.0) : 0.0);

        if (i >= 1)     v -= f.a[1] * y[i - 1];
        if (i >= 2)     v -= f.a[2] * y[i - 2];

        y[i] = v;
    }

    return y;
}

fx::BandType typeOfCode (int code)
{
    static const fx::BandType map[] = { fx::BandType::LowShelf, fx::BandType::HighShelf, fx::BandType::PeakAlt2, fx::BandType::LowPass, fx::BandType::HighPass,
                                        fx::BandType::AllPass, fx::BandType::Notch, fx::BandType::BandPass, fx::BandType::Peak, fx::BandType::Peak,
                                        fx::BandType::ParallelBandPass };
    return map[code];
}

std::vector<float> eqImpulse (const fx::EqParams& p, size_t n, double fs, int block = 512)
{
    fx::Eq eq;
    eq.prepare (fs);
    eq.setParams (p);
    std::vector<float> x (n, 0.0f);
    x[0] = 1.0f;

    for (size_t at = 0; at < n; at += (size_t) block)
    {
        float* io[1] = { x.data() + at };
        eq.process (io, 1, (int) std::min ((size_t) block, n - at));
    }

    return x;
}

//------------------------------------------------------------------------------ ReaDelay
Planar delayRun (const fx::ReaDelayParams& p, Planar in, int block = 480)
{
    fx::ReaDelay d;
    d.prepare (48000.0);
    d.setParams (p);
    const size_t n = in[0].size();

    for (size_t at = 0; at < n; at += (size_t) block)
    {
        float* io[2] = { in[0].data() + at, in[1].data() + at };
        d.process (io, 2, (int) std::min ((size_t) block, n - at));
    }

    return in;
}

/** The impulse of the measurements: L = 1.0, R = 0.25. */
Planar impulseLR (size_t n)
{
    Planar x (2, std::vector<float> (n, 0.0f));
    x[0][0] = 1.0f;
    x[1][0] = 0.25f;
    return x;
}

fx::ReaDelayParams singleTap (double timeMs, double musical = 0.0)
{
    fx::ReaDelayParams p;
    p.taps[0].enabled = true;
    p.taps[0].timeMs = timeMs;
    p.taps[0].musical = musical;

    for (size_t t = 1; t < 4; ++t)
        p.taps[t].enabled = false;

    p.wet = 0.500035;           // the wet gain of the measurements (-6 dB default as 2 n)
    p.dry = 1.0;
    p.bpm = 120.0;
    return p;
}

//------------------------------------------------------------------------------ item fades in a render
std::string fadeRpp (const std::string& fadeLines)
{
    return "<REAPER_PROJECT 0.1 \"7.0/macOS-arm64\" 1700000000\n  PANLAW 1\n  PANMODE 3\n  TEMPO 120 4 4 0\n"
           "  <TRACK {AAAAAAAA-0000-0000-0000-0000000000A1}\n    NAME \"T\"\n    VOLPAN 1 0 -1 -1 1\n    ISBUS 0 0\n"
           "    <ITEM\n      POSITION 0\n      LENGTH 1\n" + fadeLines + "\n      <SOURCE WAVE\n        FILE \"item.wav\"\n      >\n    >\n  >\n>\n";
}

/** Imports a project with one 1 s item of constant 0.5 and the given fade lines, renders the master, returns the left channel. */
std::vector<float> renderFades (test::Env& env, const std::string& fadeLines, double& rate)
{
    const auto dir = test::makeTempDir ("parityfades");
    test::writeWav (dir / "item.wav", { std::vector<float> (48000, 0.5f) }, 48000, 24);
    std::ofstream (dir / "t.RPP") << fadeRpp (fadeLines);
    const auto id = env.newProject ("fades");
    REQUIRE (env.run (id, "import.rpp", { { "path", (dir / "t.RPP").string() } }).ok);
    RenderRequest rq;
    rq.tailS = 0.0;
    auto rr = renderProject (env.project (id), rq);
    REQUIRE (rr.ok());
    std::vector<std::vector<float>> ch;
    REQUIRE (test::readWav (env.project (id).dir() / rr.value()["file"]["path"].get<std::string>(), ch, rate));
    REQUIRE (! ch.empty());
    return ch[0];
}
} // namespace

//==============================================================================
// Fades
//==============================================================================
ROVOX_TEST (parity, fade_curve_follows_the_spec_formulas_and_a_few_measured_gains)
{
    // measured: shape 0, curve -1 is t^4, -0.5 is t^2, +0.5 is 1-(1-t)^2, +1 is 1-(1-t)^4
    CHECK_NEAR (fades::gainAt (fades::normalise (0, -1.0), 0.5), 0.0625, 1e-7);
    CHECK_NEAR (fades::gainAt (fades::normalise (0, -1.0), 0.75), 0.3164062, 1e-6);
    CHECK_NEAR (fades::gainAt (fades::normalise (0, -0.9), 0.25), 0.015625, 1e-6);
    CHECK_NEAR (fades::gainAt (fades::normalise (0, 0.0), 0.3), 0.3, 1e-12);

    for (int shape : { 0, 5, 6 })
        for (double c : { -1.0, -0.9, -0.75, -0.5, -0.25, -0.1, 0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0 })
            for (int k = 0; k <= 40; ++k)
            {
                const double t = k / 40.0;
                CHECK_NEAR (fades::gainAt (fades::normalise (shape, c), t), specFade (shape, c, t), 1e-12);
            }

    // shapes 1..4 are presets of shape 0 (curve +0.5, -0.5, +1, -1); an explicit curve wins over the preset
    const double presets[4] = { 0.5, -0.5, 1.0, -1.0 };

    for (int s = 1; s <= 4; ++s)
    {
        CHECK (fades::normalise (s, 0.0) == fc (0, presets[s - 1]));

        for (int k = 0; k <= 20; ++k)
            CHECK_NEAR (fades::gainAt (fades::normalise (s, 0.0), k / 20.0), specFade (0, presets[s - 1], k / 20.0), 1e-12);
    }

    CHECK (fades::normalise (3, -0.25) == fc (0, -0.25));
    CHECK (fades::isLinear (fades::normalise (0, 0.0)));
    CHECK (! fades::isLinear (fades::normalise (1, 0.0)));
    CHECK_NEAR (fades::gainAt (fades::normalise (0, 0.3), 0.0), 0.0, 1e-15);
    CHECK_NEAR (fades::gainAt (fades::normalise (6, -0.4), 1.0), 1.0, 1e-15);
}

ROVOX_TEST (parity, fade_fields_of_project_lines_are_read_the_way_reaper_writes_them)
{
    // fields: code, amount (5), flag (7), explicit curve (8), explicit shape (9)
    CHECK (fades::fromProjectFields (1, 0, 1, 0, 0) == fc (0, 0.5));         // REAPER's default fade
    CHECK (fades::fromProjectFields (0, 0, 1, 0, 0) == fc (0, 0.0));
    CHECK (fades::fromProjectFields (2, 1, 1, 0, 0) == fc (0, -0.5));
    CHECK (fades::fromProjectFields (3, -1, 1, 0, 0) == fc (0, 1.0));
    CHECK (fades::fromProjectFields (4, 1, 1, 0, 0) == fc (0, -1.0));
    CHECK (fades::fromProjectFields (2, 0.5, 1, 0, 0) == fc (0, -0.25));
    CHECK (fades::fromProjectFields (3, -0.767857, 0, 0.75, 0) == fc (0, 0.75));
    CHECK (fades::fromProjectFields (5, 0, 0, 0, 0.5) == fc (5, 0.0));
    CHECK (fades::fromProjectFields (5.1, -0.700367, 0, 0.5, 1.0) == fc (6, 0.5));
    CHECK (fades::fromProjectFields (5.1, 0, 1, 0, 0) == fc (6, 0.0));
}

ROVOX_TEST (parity, imported_item_fades_render_the_spec_curves_and_the_fade_out_mirrors)
{
    test::Env env;
    struct Case { const char* name; std::string lines; fades::FadeCurve in, out; };
    const Case cases[] = {
        { "default code 1", "      FADEIN 1 0.25 0 1 0 0 0 1 0 0 1 0 0\n      FADEOUT 1 0.25 0 1 0 0 0 1 0 0 1 0 0", { 0, 0.5 }, { 0, 0.5 } },
        { "shape 3, shape 5", "      FADEIN 3 0.25 0 3 0 -1 -1 1 0 0 1 0 0\n      FADEOUT 5 0.25 0 5 0 0 0 0 0 0.5 1 0 0", { 0, 1.0 }, { 5, 0.0 } },
        { "shape 0 curve 0.75, shape 6 curve 0.5", "      FADEIN 3 0.25 0 3 0 -0.767857 -0.767857 0 0.75 0 1 0 0\n      FADEOUT 5.1 0.25 0 5.1 0 0.700367 0.700367 0 0.5 1 1 0 0",
          { 0, 0.75 }, { 6, 0.5 } },
        { "linear", "      FADEIN 0 0.25 0 0 0 0 0 1 0 0 1 0 0\n      FADEOUT 0 0.25 0 0 0 0 0 1 0 0 1 0 0", { 0, 0.0 }, { 0, 0.0 } } };

    for (auto& c : cases)
    {
        std::cerr << "    fades: " << c.name << "\n";
        double rate = 0.0;
        const auto out = renderFades (env, c.lines, rate);
        const double N = rate, L = 0.25 * rate;        // a 1 s item with 0.25 s fades, whatever the project rate
        REQUIRE ((double) out.size() >= N - 2.0);
        double worst = 0.0;
        size_t worstAt = 0;

        for (size_t k = 0; k < std::min<size_t> (out.size(), (size_t) N - 8); ++k)        // the last samples are the source resampler running out
        {
            double g = 1.0;

            if ((double) k < L)
                g = fades::gainAt (c.in, (double) k / L);
            else if ((double) k >= N - L)
                g = fades::gainAt (c.out, 1.0 - ((double) k - (N - L)) / L);

            const double e = std::abs ((double) out[k] - 0.5 * g);

            if (e > worst)
            {
                worst = e;
                worstAt = k;
            }
        }

        if (worst >= 2.0e-6)
            std::cerr << "    worst error " << worst << " at " << worstAt << " of " << out.size() << " (L " << L << "): got " << out[worstAt] << "\n";

        CHECK (worst < 2.0e-6);
        CHECK (std::abs (out[0]) < 1.0e-9f);                      // the first sample of the item has gain 0
    }
}

//==============================================================================
// ReaEQ
//==============================================================================
ROVOX_TEST (parity, reaeq_every_band_type_has_the_specified_impulse_response)
{
    const double fs = 48000.0;
    double worst = 0.0;

    for (int code = 0; code <= 10; ++code)
        for (double f0 : { 100.0, 1000.0, 10000.0 })
            for (double g : { 6.0, -12.0 })
                for (double bw : { 0.5, 1.0, 2.0, 4.0 })
                {
                    fx::EqParams p;
                    p.reaperCompat = true;
                    p.bands[0] = { true, typeOfCode (code), f0, g, bw };

                    for (size_t b = 1; b < 8; ++b)
                        p.bands[b].enabled = false;

                    const auto got = eqImpulse (p, 2048, fs);
                    const auto want = iirImpulse (specBand (code, f0, g, bw, fs), 2048);

                    for (size_t i = 0; i < want.size(); ++i)
                        worst = std::max (worst, std::abs ((double) got[i] - want[i]));
                }

    CHECK (worst < 3.0e-6);
}

ROVOX_TEST (parity, reaeq_matches_measured_impulse_samples)
{
    // measured at 48 kHz, f0 = 100 Hz, BW 0.495 oct; the gain is -12.05 dB for the gain types and 5.95 dB (ignored) for the filters
    struct Case { int code; double gain, ir0, ir1; };
    const Case cases[] = { { 0, -12.05, 0.9941792, -0.0116326 }, { 1, -12.05, 0.251209, 0.002939 }, { 2, -12.05, 0.998312, -0.003369 },
                           { 3, 5.95, 4.3e-05, 0.000171 }, { 4, 5.95, 0.997707, -0.004661 }, { 5, 5.95, 0.995499, -0.008981 },
                           { 6, 5.95, 0.997749, -0.00449 }, { 7, 5.95, 0.00225, 0.00449 }, { 8, -12.05, 0.996629, -0.006711 },
                           { 9, -12.05, 0.996629, -0.006711 }, { 10, 5.95, 0.00225, 0.00449 } };

    for (auto& c : cases)
    {
        fx::EqParams p;
        p.reaperCompat = true;
        p.bands[0] = { true, typeOfCode (c.code), 100.0, c.gain, 0.495 };

        for (size_t b = 1; b < 8; ++b)
            p.bands[b].enabled = false;

        const auto ir = eqImpulse (p, 64, 48000.0);
        CHECK_NEAR (ir[0], c.ir0, 5.0e-5);
        CHECK_NEAR (ir[1], c.ir1, 5.0e-5);
    }
}

ROVOX_TEST (parity, reaeq_gain_is_ignored_by_pass_notch_and_band_pass_types_and_alt2_keeps_its_poles)
{
    for (int code : { 3, 4, 5, 6, 7, 10 })
    {
        fx::EqParams a, b;
        a.reaperCompat = b.reaperCompat = true;
        a.bands[0] = { true, typeOfCode (code), 800.0, 9.0, 1.0 };
        b.bands[0] = { true, typeOfCode (code), 800.0, -20.0, 1.0 };

        for (size_t k = 1; k < 8; ++k)
            a.bands[k].enabled = b.bands[k].enabled = false;

        CHECK (eqImpulse (a, 512, 48000.0) == eqImpulse (b, 512, 48000.0));
    }

    // alt 2: the denominator does not depend on the gain, the numerator uses alpha * A^2
    const auto f = specBand (2, 1000.0, -12.0, 1.0, 48000.0), g = specBand (2, 1000.0, 6.0, 1.0, 48000.0);
    CHECK_NEAR (f.a[1], g.a[1], 1e-15);
    CHECK_NEAR (f.a[2], g.a[2], 1e-15);

    // shelf slope: BW below 0.9129 oct all give S = 1.2, above it S = 1 / BW^2
    fx::EqParams p1, p2;
    p1.reaperCompat = p2.reaperCompat = true;
    p1.bands[0] = { true, fx::BandType::LowShelf, 500.0, 6.0, 0.3 };
    p2.bands[0] = { true, fx::BandType::LowShelf, 500.0, 6.0, 0.8 };

    for (size_t k = 1; k < 8; ++k)
        p1.bands[k].enabled = p2.bands[k].enabled = false;

    CHECK (eqImpulse (p1, 512, 48000.0) == eqImpulse (p2, 512, 48000.0));
}

ROVOX_TEST (parity, reaeq_parallel_band_passes_are_summed_and_then_run_in_series_with_the_other_bands)
{
    const double fs = 48000.0;
    fx::EqParams p;
    p.reaperCompat = true;
    p.bands[0] = { true, fx::BandType::ParallelBandPass, 300.0, 0.0, 1.0 };
    p.bands[1] = { true, fx::BandType::ParallelBandPass, 2500.0, 0.0, 2.0 };
    p.bands[2] = { true, fx::BandType::Peak, 1000.0, 6.0, 1.0 };

    for (size_t k = 3; k < 8; ++k)
        p.bands[k].enabled = false;

    const auto got = eqImpulse (p, 4096, fs);
    const auto a = iirImpulse (specBand (10, 300.0, 0.0, 1.0, fs), 4096), b = iirImpulse (specBand (10, 2500.0, 0.0, 2.0, fs), 4096);
    std::vector<double> sum (4096);

    for (size_t i = 0; i < sum.size(); ++i)
        sum[i] = a[i] + b[i];

    // series with the peak: run the summed response through the peak filter
    const auto peak = specBand (8, 1000.0, 6.0, 1.0, fs);
    std::vector<double> want (4096, 0.0);

    for (size_t i = 0; i < want.size(); ++i)
    {
        double v = peak.b[0] * sum[i] + (i >= 1 ? peak.b[1] * sum[i - 1] : 0.0) + (i >= 2 ? peak.b[2] * sum[i - 2] : 0.0);

        if (i >= 1)     v -= peak.a[1] * want[i - 1];
        if (i >= 2)     v -= peak.a[2] * want[i - 2];

        want[i] = v;
    }

    double worst = 0.0;

    for (size_t i = 0; i < want.size(); ++i)
        worst = std::max (worst, std::abs ((double) got[i] - want[i]));

    CHECK (worst < 3.0e-6);
}

ROVOX_TEST (parity, the_rovox_design_of_the_original_band_types_is_unchanged_without_the_reaper_mode)
{
    // RBJ peak with Q = sqrt(2^bw) / (2^bw - 1), alpha = sin(w0) / (2 Q)
    const double fs = 48000.0, f0 = 1000.0, g = 6.0, bw = 1.0;
    const double w0 = 2 * pi * f0 / fs, q = std::sqrt (std::pow (2.0, bw)) / (std::pow (2.0, bw) - 1), alpha = std::sin (w0) / (2 * q), A = std::pow (10.0, g / 40);
    const Iir want { { (1 + alpha * A) / (1 + alpha / A), -2 * std::cos (w0) / (1 + alpha / A), (1 - alpha * A) / (1 + alpha / A) },
                     { 1, -2 * std::cos (w0) / (1 + alpha / A), (1 - alpha / A) / (1 + alpha / A) } };
    fx::EqParams p;
    p.bands[0] = { true, fx::BandType::Peak, f0, g, bw };

    for (size_t k = 1; k < 8; ++k)
        p.bands[k].enabled = false;

    const auto got = eqImpulse (p, 1024, fs);
    const auto ref = iirImpulse (want, 1024);
    double worst = 0.0;

    for (size_t i = 0; i < ref.size(); ++i)
        worst = std::max (worst, std::abs ((double) got[i] - ref[i]));

    CHECK (worst < 3.0e-6);
}

//==============================================================================
// ReaDelay
//==============================================================================
ROVOX_TEST (parity, readelay_delay_is_time_plus_musical_eighths_in_whole_samples)
{
    const auto at = [] (const fx::ReaDelayParams& p, size_t where, double expectL)
    {
        const auto out = delayRun (p, impulseLR (30000));
        CHECK_NEAR (out[0][where], expectL, 1e-5);
        CHECK_NEAR (out[1][where], expectL * 0.25, 1e-5);

        for (size_t i = 1; i < out[0].size(); ++i)
            if (i != where && std::abs (out[0][i]) > 0.0f)
            {
                CHECK (false);
                break;
            }
    };

    at (singleTap (0.0, 2.0), 24000, 0.500035);                     // default: 2 eighths at 120 bpm = 500 ms
    at (singleTap (99.95), 4798, 0.500035);                         // 4797.6 rounds to 4798
    at (singleTap (0.0, 0.995), 11940, 0.500035);
    at (singleTap (99.95, 0.995), 16738, 0.500035);                 // 11940 + 4798: the two add
    at (singleTap (10.05), 482, 0.500035);
    at (singleTap (10.45), 502, 0.500035);
}

ROVOX_TEST (parity, readelay_feedback_repeats_scale_by_the_feedback_amplitude)
{
    auto p = singleTap (99.95);
    p.taps[0].feedback = 0.49831;       // -6.05 dB
    const auto out = delayRun (p, impulseLR (60000));
    CHECK_NEAR (out[0][4798], 0.500035, 1e-5);
    CHECK_NEAR (out[0][9596], 0.500035 * 0.49831, 1e-5);            // measured 0.249172
    CHECK_NEAR (out[0][14394], 0.500035 * 0.49831 * 0.49831, 1e-5);
    CHECK_NEAR (out[1][9596], 0.125009 * 0.49831, 1e-5);
}

ROVOX_TEST (parity, readelay_loop_filters_are_12_db_per_octave_with_0_db_at_the_corner)
{
    // measured first samples of the echo, LP 1999.5 Hz and HP 999.5 Hz (wet gain 0.500035, impulse 1.0)
    {
        auto p = singleTap (99.95);
        p.taps[0].lowPassHz = 1999.5;
        const auto out = delayRun (p, impulseLR (6000));
        const double want[6] = { 0.007539, 0.027976, 0.049582, 0.063248, 0.069968, 0.070928 };

        for (size_t k = 0; k < 6; ++k)
            CHECK_NEAR (out[0][4798 + k], want[k], 2e-6);
    }
    {
        auto p = singleTap (99.95);
        p.taps[0].highPassHz = 999.5;
        const auto out = delayRun (p, impulseLR (6000));
        const double want[4] = { 0.467408, -0.064745, -0.063277, -0.060973 };

        for (size_t k = 0; k < 4; ++k)
            CHECK_NEAR (out[0][4798 + k], want[k], 2e-6);
    }
    {
        // the filter acts on every pass: the second echo is feedback * (h * h), h = the 2 kHz low-pass (RBJ, Q = 1) impulse response
        auto p = singleTap (50.0);
        p.taps[0].lowPassHz = 2000.0;
        p.taps[0].feedback = 0.5;
        const auto out = delayRun (p, impulseLR (12000));
        const double w0 = 2 * pi * 2000.0 / 48000.0, c = std::cos (w0), al = std::sin (w0) / 2;
        const Iir lp { { (1 - c) / 2 / (1 + al), (1 - c) / (1 + al), (1 - c) / 2 / (1 + al) }, { 1, -2 * c / (1 + al), (1 - al) / (1 + al) } };
        const auto h = iirImpulse (lp, 400);
        std::vector<double> hh (400, 0.0);

        for (size_t i = 0; i < 400; ++i)
            for (size_t k = 0; k <= i; ++k)
                hh[i] += h[k] * h[i - k];

        for (size_t k = 0; k < 40; ++k)
        {
            CHECK_NEAR (out[0][2400 + k], 0.500035 * h[k], 2e-6);
            CHECK_NEAR (out[0][4800 + k], 0.500035 * 0.5 * hh[k], 2e-6);
        }
    }
}

ROVOX_TEST (parity, readelay_resolution_width_pan_and_volume_per_tap)
{
    // resolution: truncation toward zero in steps of 2^-bits, before the gains
    {
        auto p = singleTap (10.0);
        p.taps[0].bits = 4;
        Planar x (2, std::vector<float> (2000, 0.0f));
        x[0][0] = 0.3f;
        x[1][0] = -0.3f;
        const auto out = delayRun (p, x);
        CHECK_NEAR (out[0][480], 0.25 * 0.500035, 1e-6);            // trunc (0.3 * 16) / 16
        CHECK_NEAR (out[1][480], -0.25 * 0.500035, 1e-6);
    }

    // width: L' = M + w S, R' = M - w S (M = (L + R) / 2, S = (L - R) / 2)
    for (double w : { 0.0, -1.0, 0.3, 1.0 })
    {
        auto p = singleTap (10.0);
        p.taps[0].width = w;
        const auto out = delayRun (p, impulseLR (1000));
        const double m = 0.625, s = 0.375;
        CHECK_NEAR (out[0][480], (m + w * s) * 0.500035, 1e-6);
        CHECK_NEAR (out[1][480], (m - w * s) * 0.500035, 1e-6);
    }

    // pan: balance law, positive pan scales the left channel by 1 - p, negative scales the right by 1 + p
    {
        auto p = singleTap (10.0);
        p.taps[0].pan = 0.5;
        auto out = delayRun (p, impulseLR (1000));
        CHECK_NEAR (out[0][480], 0.500035 * 0.5, 1e-6);
        CHECK_NEAR (out[1][480], 0.125009, 1e-6);
        p.taps[0].pan = -0.4;
        out = delayRun (p, impulseLR (1000));
        CHECK_NEAR (out[0][480], 0.500035, 1e-6);
        CHECK_NEAR (out[1][480], 0.125009 * 0.6, 1e-6);
    }

    // volume of the tap, wet / dry amplitudes (measured: wet 0.99426, dry 0.49831)
    {
        auto p = singleTap (99.95);
        p.taps[0].volume = 0.49831;
        auto out = delayRun (p, impulseLR (6000));
        CHECK_NEAR (out[0][4798], 0.249172, 1e-5);
        p = singleTap (99.95);
        p.wet = 0.99426;
        p.dry = 0.49831;
        out = delayRun (p, impulseLR (6000));
        CHECK_NEAR (out[0][0], 0.49831, 1e-6);
        CHECK_NEAR (out[0][4798], 0.99426, 1e-6);
    }
}

ROVOX_TEST (parity, readelay_taps_are_independent_loops_that_add_up)
{
    // unmeasured: several taps sum, every tap has its own feedback loop
    auto p = singleTap (10.0);
    p.taps[1].enabled = true;
    p.taps[1].timeMs = 25.0;
    p.taps[1].musical = 0.0;
    p.taps[1].feedback = 0.5;
    const auto out = delayRun (p, impulseLR (4000));
    CHECK_NEAR (out[0][480], 0.500035, 1e-6);
    CHECK_NEAR (out[0][1200], 0.500035, 1e-6);          // tap 2: 25 ms
    CHECK_NEAR (out[0][2400], 0.500035 * 0.5, 1e-6);    // tap 2 feedback
    CHECK_NEAR (out[0][960], 0.0, 1e-9);                // tap 1 has no feedback
}

//==============================================================================
// Translation and the generic Wet / Delta
//==============================================================================
namespace
{
json prm (int index, const char* name, double n, const char* text)
{
    return { { "index", index }, { "name", name }, { "normalized", n }, { "formatted", text } };
}

double translatedValue (const FxTranslation& t, const std::string& to, bool* found = nullptr)
{
    for (auto& x : t.translated)
        if (x.to == to)
        {
            if (found != nullptr)   *found = true;
            return x.value;
        }

    if (found != nullptr)   *found = false;
    return 0.0;
}
} // namespace

ROVOX_TEST (parity, reaeq_translation_uses_the_codes_the_exact_knob_values_and_the_reaper_mode)
{
    const json entry = { { "name", "ReaEQ" },
                         { "config", { { "BANDTYPE0", "2" }, { "BANDENABLED0", "1" }, { "BANDTYPE1", "10" }, { "BANDENABLED1", "1" }, { "BANDTYPE2", "5" }, { "BANDENABLED2", "1" } } },
                         { "params", json::array ({ prm (0, "Freq-Band 1", 0.5, "1160.5"), prm (1, "Gain-Band 1", 0.124873418, "-12.0"), prm (2, "BW-Band 1", 0.248749994, "1.00"),
                                                    prm (3, "Freq-Band 2", 0.5, "1160.5"), prm (4, "Gain-Band 2", 0.75, "8.0"), prm (5, "BW-Band 2", 0.123749997, "0.50"),
                                                    prm (6, "Freq-Band 3", 0.5, "1160.5"), prm (7, "Gain-Band 3", 0.5, "0.0"), prm (8, "BW-Band 3", 0.5, "2.00"),
                                                    prm (9, "Global Gain", 0.5, "0.0"),
                                                    prm (10, "Bypass", 0.0, "normal"), prm (11, "Wet", 0.5, "50"), prm (12, "Delta", 0.8, "enabled") }) } };
    const auto t = translateReaperFx ("eq", parseReaperFxEntry (entry));
    CHECK_NEAR (translatedValue (t, "b1_type"), 8.0, 1e-9);        // code 2 = Band (alt 2)
    CHECK_NEAR (translatedValue (t, "b2_type"), 9.0, 1e-9);        // code 10 = parallel band pass
    CHECK_NEAR (translatedValue (t, "b3_type"), 7.0, 1e-9);        // code 5 = all pass
    CHECK_NEAR (translatedValue (t, "b1_gain"), 20.0 * std::log10 (2 * 0.124873418), 1e-9);
    CHECK_NEAR (translatedValue (t, "b1_bw"), 4 * 0.248749994, 1e-9);
    CHECK_NEAR (translatedValue (t, "b2_gain"), 20.0 * std::log10 (1.0 + 6 * 0.25), 1e-9);
    CHECK_NEAR (translatedValue (t, "b2_bw"), 4 * 0.123749997, 1e-9);
    CHECK_NEAR (translatedValue (t, "reaper_compat"), 1.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "fx_wet"), 50.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "fx_delta"), 1.0, 1e-9);
    CHECK (t.untranslated.empty());
}

ROVOX_TEST (parity, readelay_translation_reads_the_knobs_exactly)
{
    const json entry = { { "name", "ReaDelay" },
                         { "params", json::array ({ prm (0, "Wet", 0.497130051, "-0.0"), prm (1, "Dry", 0.249155231, "-6.0"),
                                                    prm (2, "1: Enabled", 1.0, "ON"), prm (3, "1: Length (time)", 0.00999499997, "100.0"),
                                                    prm (4, "1: Length (musical)", 0.00388671865, "1.00"), prm (5, "1: Feedback", 0.249155231, "-6.0"),
                                                    prm (6, "1: Lowpass", 0.0999750011, "2000"), prm (7, "1: Hipass", 0.0499749985, "1000"),
                                                    prm (8, "1: Resolution", 0.249999993, "8"), prm (9, "1: Stereo width", 0.497499987, "-0.00"),
                                                    prm (10, "1: Volume", 0.249155231, "-6.0"), prm (11, "1: Pan", 0.749749988, "0.500"),
                                                    prm (12, "Bypass", 0.0, "normal"), prm (13, "Wet", 1.0, "100"), prm (14, "Delta", 0.0, "normal") }) } };
    const auto t = translateReaperFx ("delay", parseReaperFxEntry (entry));
    CHECK_NEAR (translatedValue (t, "t1_time"), 99.95, 1e-6);
    CHECK_NEAR (translatedValue (t, "t1_musical"), 0.995, 1e-6);
    CHECK_NEAR (translatedValue (t, "t1_fb"), 20.0 * std::log10 (2 * 0.249155231), 1e-9);
    CHECK_NEAR (translatedValue (t, "t1_lp"), 1999.5, 1e-3);
    CHECK_NEAR (translatedValue (t, "t1_hp"), 999.5, 1e-3);
    CHECK_NEAR (translatedValue (t, "t1_bits"), 8.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "t1_width"), 2 * 0.497499987 - 1, 1e-9);
    CHECK_NEAR (translatedValue (t, "t1_pan"), 2 * 0.749749988 - 1, 1e-9);
    CHECK_NEAR (translatedValue (t, "t1_gain"), 20.0 * std::log10 (2 * 0.249155231), 1e-9);
    CHECK_NEAR (translatedValue (t, "wet"), 20.0 * std::log10 (2 * 0.497130051), 1e-9);
    CHECK_NEAR (translatedValue (t, "dry"), 20.0 * std::log10 (2 * 0.249155231), 1e-9);
    CHECK_NEAR (translatedValue (t, "reaper_compat"), 1.0, 1e-9);
    bool found = true;
    translatedValue (t, "fx_delta", &found);
    CHECK (! found);
}

ROVOX_TEST (parity, generic_wet_is_a_linear_crossfade_and_delta_is_processed_minus_input)
{
    test::Env env;
    const auto id = env.newProject ("mix");
    const auto dir = test::makeTempDir ("paritymix");
    test::writeWav (dir / "sine.wav", { test::sine (330.0, -12.0, 1.0, 48000.0) }, 48000.0, 32);
    auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "sine.wav").string() } } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    const auto track = env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();
    const auto eq = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "rovox:eq" } } } })["plugin_id"].get<std::string>();
    env.ok (id, "fx.update", { { "plugin_id", eq }, { "params", { { "reaper_compat", 1 }, { "b2_gain", 9.0 }, { "b2_freq", 330.0 }, { "b2_bw", 1.0 } } } });

    int n = 0;
    const auto render = [&]
    {
        RenderRequest rq;
        rq.name = "mix" + std::to_string (n++);
        rq.tailS = 0.0;
        auto r = renderProject (env.project (id), rq);
        REQUIRE (r.ok());
        std::vector<std::vector<float>> ch;
        double sr = 0;
        REQUIRE (test::readWav (env.project (id).rendersDir() / (rq.name + ".wav"), ch, sr));
        return ch[0];
    };

    const auto processed = render();
    env.ok (id, "fx.update", { { "plugin_id", eq }, { "enabled", false } });
    const auto dry = render();                                  // bypassed: the input exactly
    env.ok (id, "fx.update", { { "plugin_id", eq }, { "enabled", true }, { "params", { { "fx_wet", 25.0 } } } });
    const auto wet = render();
    env.ok (id, "fx.update", { { "plugin_id", eq }, { "params", { { "fx_wet", 25.0 }, { "fx_delta", 1 } } } });
    const auto delta = render();
    REQUIRE (processed.size() == dry.size());
    REQUIRE (wet.size() == dry.size());
    REQUIRE (delta.size() == dry.size());
    double worstWet = 0.0, worstDelta = 0.0, diff = 0.0;

    for (size_t i = 4800; i < dry.size() - 4800; ++i)
    {
        worstWet = std::max (worstWet, std::abs ((double) wet[i] - (0.25 * processed[i] + 0.75 * dry[i])));
        worstDelta = std::max (worstDelta, std::abs ((double) delta[i] - ((double) processed[i] - dry[i])));       // the Wet is ignored
        diff = std::max (diff, std::abs ((double) processed[i] - dry[i]));
    }

    CHECK (diff > 0.05);
    CHECK (worstWet < 2.0e-6);
    CHECK (worstDelta < 2.0e-6);
}

//==============================================================================
// ReaGate, ReaComp, ReaLimit
//==============================================================================
namespace
{
struct Seg { double db; double seconds; };

std::vector<float> dcSegments (const std::vector<Seg>& segs, double fs, std::vector<size_t>* starts = nullptr)
{
    std::vector<float> x;

    for (auto& s : segs)
    {
        if (starts != nullptr)
            starts->push_back (x.size());

        x.insert (x.end(), (size_t) std::lround (s.seconds * fs), (float) std::pow (10.0, s.db / 20.0));
    }

    return x;
}

/** Runs a mono signal through a processor in 512-sample blocks; the output is shifted back by the latency (the host compensates it). */
template <class Fx>
std::vector<float> runMono (Fx& fxp, std::vector<float> in, int latency)
{
    const size_t n = in.size();
    in.insert (in.end(), (size_t) latency, 0.0f);

    for (size_t at = 0; at < in.size(); at += 512)
    {
        float* io[1] = { in.data() + at };
        fxp.process (io, 1, (int) std::min<size_t> (512, in.size() - at));
    }

    return std::vector<float> (in.begin() + latency, in.begin() + latency + (long) n);
}

double dbOf (double v)      { return 20.0 * std::log10 (std::max (std::abs (v), 1.0e-12)); }

//------------------------------------------------------------------------------ ReaGate
const std::vector<Seg> gateSegments { { -40, 0.3 }, { -10, 0.5 }, { -40, 0.5 }, { -23, 0.3 }, { -17, 0.3 }, { -23, 0.3 }, { -40, 0.5 } };

fx::ReaGateParams gateParams (double attackMs, double releaseMs)
{
    fx::ReaGateParams p;
    p.thresholdDb = -20.05;
    p.attackMs = attackMs;
    p.releaseMs = releaseMs;
    return p;
}

struct GateRun
{
    std::vector<float> in, out;
    std::vector<size_t> starts;
    double gain (size_t segment, long offset) const
    {
        const size_t i = (size_t) ((long) starts[segment] + offset);
        return (double) out[i] / (double) in[i];
    }
};

GateRun runGate (const fx::ReaGateParams& p, const std::vector<Seg>& segs = gateSegments)
{
    GateRun r;
    r.in = dcSegments (segs, 48000.0, &r.starts);
    fx::ReaGate g;
    g.prepare (48000.0);
    g.setParams (p);
    r.out = runMono (g, r.in, g.latencySamples());
    return r;
}
} // namespace

ROVOX_TEST (parity, reagate_closed_gain_is_zero_attack_and_release_are_linear_ramps_of_the_offset_length)
{
    // attack 1 ms: Na = 48 - 24 samples, g[n] = (n + 1) / Na; hold = Na - 1 samples; release 50 ms: Nr = 2400 - 24, g falls by 1 / Nr per sample
    const auto r = runGate (gateParams (1.0, 50.0));
    const double Na = 24.0, Nr = 2376.0;
    CHECK (r.gain (0, 100) == 0.0);

    for (long n : { 0L, 1L, 2L, 10L, 23L, 100L })
        CHECK_NEAR (r.gain (1, n), std::min (1.0, (double) (n + 1) / Na), 1e-6);

    CHECK_NEAR (r.gain (1, 0), 0.041667, 1e-6);                     // measured
    CHECK_NEAR (r.gain (1, 10), 0.458333, 1e-6);

    for (long n : { 0L, 10L, 22L })
        CHECK_NEAR (r.gain (2, n), 1.0, 1e-6);

    for (long n : { 23L, 24L, 47L, 455L, 1000L, 2376L, 2397L })
        CHECK_NEAR (r.gain (2, n), std::max (0.0, 1.0 - (double) (n - 22) / Nr), 2e-6);

    CHECK_NEAR (r.gain (2, 24), 0.999158, 1e-6);                    // measured
    CHECK_NEAR (r.gain (2, 455), 0.817761, 1e-6);
    CHECK_NEAR (r.gain (2, 2376), 0.009259, 1e-6);
    CHECK (r.gain (2, 4776) == 0.0);
    CHECK (r.gain (3, 0) == 0.0);                                   // -23 dB is below the threshold of the closed gate
}

ROVOX_TEST (parity, reagate_hold_is_the_longer_of_hold_and_attack_less_the_offset)
{
    const auto a10 = runGate (gateParams (10.0, 100.0));
    CHECK_NEAR (a10.gain (1, 0), 0.002193, 1e-6);                   // 1 / 456
    CHECK_NEAR (a10.gain (1, 100), 0.221491, 1e-6);
    CHECK_NEAR (a10.gain (1, 455), 1.0, 1e-6);
    CHECK_NEAR (a10.gain (2, 454), 1.0, 1e-9);                      // hold 480 - 24 - 1 samples
    CHECK_NEAR (a10.gain (2, 455), 0.999791, 1e-6);
    CHECK_NEAR (a10.gain (2, 4776), 0.095059, 1e-6);                // release ramp 1 / 4776

    auto p = gateParams (0.0, 0.0);
    p.holdMs = 50.0;
    const auto h = runGate (p);
    CHECK_NEAR (h.gain (2, 2375), 1.0, 1e-9);
    CHECK (h.gain (2, 2376) == 0.0);                                // closes at sample 2376 (measured)
    p.releaseMs = 100.0;
    const auto hr = runGate (p);
    CHECK_NEAR (hr.gain (2, 2376), 0.999791, 1e-6);
    CHECK_NEAR (hr.gain (2, 4776), 0.497278, 1e-6);
    CHECK_NEAR (hr.gain (2, 5000), 0.450377, 1e-6);

    const auto instant = runGate (gateParams (0.0, 0.0));
    CHECK_NEAR (instant.gain (1, 0), 1.0, 1e-9);
    CHECK (instant.gain (2, 0) == 0.0);
}

ROVOX_TEST (parity, reagate_closes_at_threshold_plus_hysteresis)
{
    auto p = gateParams (0.0, 0.0);
    const auto plain = runGate (p);
    CHECK_NEAR (plain.gain (4, 0), 1.0, 1e-9);                      // -17 opens
    CHECK (plain.gain (5, 0) == 0.0);                               // -23 closes

    p.hysteresisDb = -6.0;
    const auto neg = runGate (p);
    CHECK (neg.gain (3, 0) == 0.0);                                 // still closed: opening needs the threshold itself
    CHECK_NEAR (neg.gain (5, 0), 1.0, 1e-9);                        // stays open down to threshold - 6 dB
    CHECK_NEAR (neg.gain (5, 5000), 1.0, 1e-9);
    CHECK (neg.gain (6, 0) == 0.0);

    p.hysteresisDb = 3.0;
    const auto pos = runGate (p);
    CHECK_NEAR (pos.gain (4, 0), 1.0, 1e-9);
    CHECK (pos.gain (5, 0) == 0.0);
}

ROVOX_TEST (parity, reagate_pre_open_moves_the_opening_ahead_and_leaves_the_closing_on_the_audio)
{
    auto p = gateParams (0.0, 0.0);
    p.preOpenMs = 10.0;
    fx::ReaGate g;
    g.prepare (48000.0);
    g.setParams (p);
    CHECK (g.latencySamples() == 456);                              // 10 ms - 24 samples
    const auto r = runGate (p);
    CHECK_NEAR (r.gain (1, -456), 1.0, 1e-9);
    CHECK (r.gain (1, -457) == 0.0);
    CHECK_NEAR (r.gain (1, 0), 1.0, 1e-9);
    CHECK_NEAR (r.gain (2, -1), 1.0, 1e-9);
    CHECK (r.gain (2, 0) == 0.0);                                   // closing is not advanced
    CHECK (r.gain (3, 0) == 0.0);
    CHECK_NEAR (r.gain (4, -456), 1.0, 1e-9);
}

ROVOX_TEST (parity, reagate_rms_detector_uses_a_boxcar_window_of_the_size_less_the_offset)
{
    auto p = gateParams (0.0, 0.0);
    p.rmsMs = 10.0;
    const auto r = runGate (p);
    CHECK (r.gain (1, 24) == 0.0);                                  // measured
    CHECK_NEAR (r.gain (1, 47), 1.0, 1e-9);

    // window 456 samples: opens when the mean square of the window reaches the threshold (-40 dB DC before the step, -10 dB after)
    const double T2 = std::pow (10.0, -20.05 / 10.0), hi = 0.1, lo = 1.0e-4;
    long open = -1;

    for (long n = 0; n < 456 && open < 0; ++n)
        if (((double) (n + 1) * hi + (456.0 - (double) (n + 1)) * lo) / 456.0 >= T2)
            open = n;

    for (long n = 0; n < 100; ++n)
        CHECK_NEAR (r.gain (1, n), n >= open ? 1.0 : 0.0, 1e-9);

    CHECK (open >= 43 && open <= 45);                               // measured 43 after the step
    CHECK_NEAR (r.gain (2, 24), 1.0, 1e-9);
    CHECK (r.gain (2, 455) == 0.0);
}

ROVOX_TEST (parity, reagate_invert_wet_dry_and_detector_filters)
{
    auto p = gateParams (0.0, 0.0);
    p.invertWet = true;
    const auto inv = runGate (p);
    CHECK_NEAR (inv.gain (0, 100), 1.0, 1e-9);
    CHECK (inv.gain (1, 100) == 0.0);
    CHECK_NEAR (inv.gain (3, 0), 1.0, 1e-9);
    CHECK (inv.gain (4, 0) == 0.0);

    p = gateParams (0.0, 0.0);
    p.dry = 0.49831;                                                // -6.05 dB
    const auto dry = runGate (p);
    CHECK_NEAR (dry.gain (0, 100), 0.49831, 1e-6);
    CHECK_NEAR (dry.gain (1, 100), 1.49831, 1e-6);

    // the detector high-pass (12 dB/oct, 2 kHz) keeps a DC step below the threshold once it has settled; the audio is not filtered
    p = gateParams (0.0, 0.0);
    p.highPassHz = 2000.0;
    const auto hp = runGate (p);
    CHECK (hp.gain (1, 20000) == 0.0);
    CHECK_NEAR (hp.out[hp.starts[1] + 100], hp.in[hp.starts[1] + 100] * hp.gain (1, 100), 1e-9);
}

//------------------------------------------------------------------------------ ReaComp
namespace
{
fx::ReaCompParams compParams (double thresholdDb, double attackMs, double releaseMs)
{
    fx::ReaCompParams p;
    p.thresholdDb = thresholdDb;
    p.ratio = 100.0;            // infinite
    p.attackMs = attackMs;
    p.releaseMs = releaseMs;
    p.rmsMs = 0.0;
    p.dry = 0.0;
    p.wet = 1.0;
    return p;
}

/** The step signal of the dynamics vectors: -40 dBFS 0.5 s, -6 dBFS 1 s, -40 dBFS 1 s. Gain in dB at an offset from the step up (0) or the drop. */
struct CompRun
{
    std::vector<float> in, out;
    double up (long n) const    { return dbOf ((double) out[(size_t) (24000 + n)] / (double) in[(size_t) (24000 + n)]); }
    double down (long n) const  { return dbOf ((double) out[(size_t) (72000 + n)] / (double) in[(size_t) (72000 + n)]); }
};

CompRun runComp (const fx::ReaCompParams& p)
{
    CompRun r;
    r.in = dcSegments ({ { -40, 0.5 }, { -6, 1.0 }, { -40, 1.0 } }, 48000.0);
    fx::ReaComp c;
    c.prepare (48000.0);
    c.setParams (p);
    r.out = runMono (c, r.in, c.latencySamples());
    return r;
}
} // namespace

ROVOX_TEST (parity, reacomp_hard_knee_static_curve_has_slope_one_over_ratio)
{
    // measured gains (dB) of a DC staircase; the threshold is -20.05 dB
    struct Pt { double ratio, level, gain; };
    const Pt pts[] = { { 2, -18, -1.024 }, { 2, -10, -5.022 }, { 2, 0, -10.02 }, { 4, -20, -0.037 }, { 4, -10, -7.534 }, { 4, 0, -15.031 },
                       { 100, -10, -10.049 }, { 100, -6, -14.049 }, { 100, 0, -20.048 }, { 4, -30, 0.0 }, { 2, -22, 0.0 } };

    for (auto& pt : pts)
    {
        auto p = compParams (-20.05, 1.0, 20.0);
        p.ratio = pt.ratio;
        CHECK_NEAR (fx::ReaComp::staticGainDb (p, pt.level), pt.gain, 0.012);
    }

    auto p = compParams (-20.05, 1.0, 20.0);
    p.ratio = 4.0;
    CHECK_NEAR (fx::ReaComp::staticGainDb (p, -10.05), -7.5, 1e-9);      // exactly (1 - 1/R) * over
}

ROVOX_TEST (parity, reacomp_soft_knee_reproduces_the_measured_tables_and_is_smooth_and_monotone)
{
    struct Pt { double w; bool legacy; double over, gr; };
    const Pt pts[] = { { 3, false, -2, 0.4078 }, { 3, false, 0, 1.3502 }, { 3, false, 2, 2.4076 },
                       { 6, false, -5, 0.3301 }, { 6, false, -3, 1.1039 }, { 6, false, 0, 2.4749 }, { 6, false, 3, 4.1036 }, { 6, false, 5, 5.3296 },
                       { 12, false, -11, 0.2001 }, { 12, false, -6, 1.572 }, { 12, false, 0, 4.0637 }, { 12, false, 6, 7.5714 }, { 12, false, 11, 11.199 },
                       { 24, false, -23, 0.0597 }, { 24, false, -12, 1.4119 }, { 24, false, 0, 5.4856 }, { 24, false, 12, 13.4107 },
                       { 12, true, -11, 0.0942 }, { 12, true, -6, 0.8015 }, { 12, true, 0, 2.4099 }, { 12, true, 6, 5.6192 }, { 12, true, 11, 10.6265 } };

    for (auto& pt : pts)
    {
        fx::ReaCompParams p;
        p.thresholdDb = 0.0;
        p.ratio = 100.0;
        p.kneeDb = pt.w;
        p.legacyKnee = pt.legacy;
        CHECK_NEAR (fx::ReaComp::staticGainDb (p, pt.over), -pt.gr, 0.05);      // within 0.1 dB of the measurements, in practice 0.01
    }

    // the knee spans threshold -+ W, reduction ratio dependence, and the centre value of the default knee for W between the tables
    // (spec: the gain at the threshold is the mean of the linear gains at the ends of the knee)
    for (double w : { 1.5, 4.5, 9.0, 18.0 })
    {
        fx::ReaCompParams p;
        p.ratio = 100.0;
        p.kneeDb = w;
        const double centre = -fx::ReaComp::staticGainDb (p, 0.0), want = -20.0 * std::log10 (0.5 * (1.0 + std::pow (10.0, -w / 20.0)));
        CHECK_NEAR (centre, want, 0.06);
        CHECK (fx::ReaComp::staticGainDb (p, -w - 0.01) == 0.0);
        CHECK_NEAR (fx::ReaComp::staticGainDb (p, w), -w, 1e-9);
        CHECK_NEAR (fx::ReaComp::staticGainDb (p, w + 5.0), -(w + 5.0), 1e-9);

        double last = 0.0, lastSlope = 0.0;

        for (int k = -(int) (w * 40) - 4; k <= (int) (w * 40) + 4; ++k)
        {
            const double x = k / 20.0, gr = -fx::ReaComp::staticGainDb (p, x);
            CHECK (gr >= last - 1e-12);                             // never falls
            const double slope = (gr - last) * 20.0;

            if (x > -w + 0.5 && x < w - 0.5)
                CHECK (std::abs (slope - lastSlope) < 0.25);        // no kinks
            last = gr;
            lastSlope = slope;
        }
    }

    // gain reduction in proportion to 1 - 1/R inside the knee (R = 4, W = 12)
    fx::ReaCompParams q;
    q.thresholdDb = -20.05;
    q.ratio = 4.0;
    q.kneeDb = 12.0;
    CHECK_NEAR (fx::ReaComp::staticGainDb (q, -30.0), -0.331, 0.02);
    CHECK_NEAR (fx::ReaComp::staticGainDb (q, -20.0), -3.066, 0.02);
    CHECK_NEAR (fx::ReaComp::staticGainDb (q, -14.0), -5.702, 0.02);
}

ROVOX_TEST (parity, reacomp_envelope_attacks_from_the_threshold_and_releases_toward_the_input)
{
    // threshold -30.05, ratio inf, attack 1 ms, release 50 ms; the measured gains (dB) after the step up and after the drop
    const auto r = runComp (compParams (-30.05, 1.0, 50.0));
    const struct { long n; double db; } up[] = { { 0, -2.3526 }, { 1, -4.1671 }, { 2, -5.6385 }, { 5, -8.8556 }, { 10, -12.2276 }, { 24, -17.0381 }, { 48, -20.5145 },
                                                { 96, -22.9204 }, { 240, -23.9964 }, { 480, -24.0473 }, { 4800, -24.0476 } };
    const struct { long n; double db; } down[] = { { 0, -24.044 }, { 24, -23.958 }, { 240, -23.1851 }, { 960, -20.6208 }, { 2400, -15.5688 }, { 4800, -7.5641 }, { 9600, 0.0 } };

    for (auto& u : up)
        CHECK_NEAR (r.up (u.n), u.db, 0.1);

    for (auto& d : down)
        CHECK_NEAR (r.down (d.n), d.db, 0.2);

    // the spec formulas themselves: e starts at the threshold level on attack, v + a (e - v)
    const double a = std::exp (-1.0 / 48.0), T = std::pow (10.0, -30.05 / 20.0), v = std::pow (10.0, -6.0 / 20.0);
    const double e0 = v + a * (T - v);
    CHECK_NEAR (r.up (0), -(20.0 * std::log10 (e0) + 30.05), 1e-3);
}

ROVOX_TEST (parity, reacomp_rms_detector_pre_comp_make_up_and_mix)
{
    // boxcar RMS of 50 ms (2400 samples): the first gain reduction after the step is at sample 8 (measured), 10 ms: at sample 1
    {
        auto p = compParams (-30.05, 0.0, 0.0);
        p.rmsMs = 50.0;
        const auto r = runComp (p);
        CHECK_NEAR (r.up (7), 0.0, 1e-3);
        CHECK (r.up (8) < -0.01);
        CHECK_NEAR (r.up (10), -1.0269, 0.05);
        CHECK_NEAR (r.up (96), -10.1608, 0.1);
        p.rmsMs = 10.0;
        const auto s = runComp (p);
        CHECK_NEAR (s.up (0), 0.0, 1e-3);
        CHECK (s.up (1) < -0.01);
    }

    // pre-comp 10 ms: latency-compensated lookahead, the reduction is complete before the loud part (measured 477 samples early)
    {
        auto p = compParams (-30.05, 0.0, 0.0);
        p.preCompMs = 10.0;
        fx::ReaComp c;
        c.prepare (48000.0);
        c.setParams (p);
        CHECK (c.latencySamples() == 480);
        const auto r = runComp (p);
        CHECK_NEAR (r.up (-470), -24.0476, 0.05);
        CHECK_NEAR (r.up (-483), 0.0, 1e-3);
        CHECK_NEAR (r.down (-100), -24.0476, 0.05);                 // the drop is not anticipated: the audio is still loud
        p.attackMs = 10.0;
        p.releaseMs = 50.0;
        const auto s = runComp (p);
        CHECK_NEAR (s.up (0), -20.3793, 0.1);                       // measured
        CHECK_NEAR (s.up (480), -22.8769, 0.1);
    }

    // auto make-up gain -T (1 - 1/R) / 2
    {
        fx::ReaCompParams p;
        p.autoMakeup = true;
        p.thresholdDb = -20.05;
        p.ratio = 4.0;
        CHECK_NEAR (fx::ReaComp::makeupDb (p), 7.516, 0.01);
        p.thresholdDb = -10.05;
        p.ratio = 2.0;
        CHECK_NEAR (fx::ReaComp::makeupDb (p), 2.511, 0.01);
        p.thresholdDb = -30.05;
        p.ratio = 100.0;
        CHECK_NEAR (fx::ReaComp::makeupDb (p), 15.02, 0.01);
        auto q = compParams (-30.05, 1.0, 50.0);
        q.autoMakeup = true;
        const auto r = runComp (q);
        CHECK_NEAR (r.up (4800), -9.0241, 0.01);
        CHECK_NEAR (r.down (9600), 15.0235, 0.01);
    }

    // dry and wet gains sum linearly (dry -6.05 dB: 0.49831)
    {
        auto p = compParams (-30.05, 1.0, 50.0);
        p.dry = 0.49831;
        const auto r = runComp (p);
        CHECK_NEAR (r.up (4800), -5.0198, 0.01);
        CHECK_NEAR (r.down (9600), 3.512, 0.01);
        p.dry = 0.0;
        p.wet = 0.49831;
        const auto w = runComp (p);
        CHECK_NEAR (w.up (4800), -24.0476 - 6.0, 0.1);
    }
}

ROVOX_TEST (parity, reacomp_detector_filters_are_12_db_per_octave_and_only_feed_the_detector)
{
    // LP 1 kHz: 0.0 dB at 1 kHz, +0.91 at 500 Hz, -11.2 at 2 kHz, -24.2 at 4 kHz
    const struct { double hz, db, tol; } lp[] = { { 1000, 0.0, 0.1 }, { 500, 0.91, 0.1 }, { 2000, -11.2, 0.15 }, { 4000, -24.2, 0.6 } };

    for (auto& pt : lp)
    {
        fx::DetectorFilter f;
        f.set (48000.0, 1000.0, 0.0);
        double peak = 0.0;

        for (int i = 0; i < 48000; ++i)
        {
            const double y = f.run (std::sin (2.0 * pi * pt.hz * i / 48000.0));

            if (i > 24000)
                peak = std::max (peak, std::abs (y));
        }

        CHECK_NEAR (dbOf (peak), pt.db, pt.tol);
    }

    // a detector high-pass of 5 kHz keeps a 100 Hz tone from compressing, the tone itself passes unchanged
    fx::ReaCompParams p;
    p.thresholdDb = -30.0;
    p.ratio = 100.0;
    p.attackMs = 0.0;
    p.releaseMs = 0.0;
    p.rmsMs = 0.0;
    p.highPassHz = 5000.0;
    fx::ReaComp c;
    c.prepare (48000.0);
    c.setParams (p);
    std::vector<float> tone (24000);

    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = (float) (0.5 * std::sin (2.0 * pi * 100.0 * (double) i / 48000.0));

    const auto out = runMono (c, tone, 0);
    double worst = 0.0;

    for (size_t i = 12000; i < tone.size(); ++i)
        worst = std::max (worst, std::abs ((double) out[i] - tone[i]));

    CHECK (worst < 2.0e-3);
}

//------------------------------------------------------------------------------ ReaLimit
namespace
{
/** -30 dBFS 0.5 s, -3 dBFS 0.5 s, -30 dBFS 1 s (the step signal of the limiter vectors). Gain relative to the boosted input, as the vectors state it. */
struct LimitRun
{
    std::vector<float> in, out;
    double boost = 1.0;
    double gain (size_t i) const    { return (double) out[i] / ((double) in[i] * boost); }
};

LimitRun runLimit (const fx::ReaLimitParams& p, std::vector<float> in)
{
    LimitRun r;
    r.in = std::move (in);
    r.boost = std::pow (10.0, -p.thresholdDb / 20.0) * std::pow (10.0, p.ceilingDb / 20.0);
    fx::ReaLimit l;
    l.prepare (48000.0);
    l.setParams (p);
    r.out = runMono (l, r.in, l.latencySamples());
    return r;
}

std::vector<float> limiterStep()
{
    return dcSegments ({ { -30, 0.5 }, { -3, 0.5 }, { -30, 1.0 } }, 48000.0);
}

/** A -60 dBFS floor with a unit impulse at 2.5 s. */
std::vector<float> limiterImpulse()
{
    std::vector<float> x (48000 * 3, 0.001f);
    x[120000] = 1.0f;
    return x;
}
} // namespace

ROVOX_TEST (parity, realimit_threshold_boosts_the_ceiling_scales_and_peaks_stay_just_below_the_ceiling)
{
    fx::ReaLimitParams p;
    p.thresholdDb = -10.0;
    p.ceilingDb = -6.0;
    p.releaseDbPerSec = 6.0;
    const auto r = runLimit (p, limiterStep());
    CHECK_NEAR (dbOf (r.out[10000] / (double) r.in[10000]), 10.0 - 6.0, 1e-3);          // -30 dBFS comes out at -26 dBFS
    CHECK_NEAR (dbOf (r.out[36000]), -6.014, 0.01);                                       // a -3 dBFS DC would be +7: -6.01 dBFS (measured)
    double peak = 0.0;

    for (float v : r.out)
        peak = std::max (peak, (double) std::abs (v));

    CHECK (dbOf (peak) < -6.0);
    CHECK (dbOf (peak) > -6.1);

    // below 0 dBFS nothing happens, a boosted peak settles 0.014 dB below the ceiling (measured -0.014 dBFS)
    p.thresholdDb = 0.0;
    p.ceilingDb = 0.0;
    const auto q = runLimit (p, limiterStep());
    CHECK_NEAR (q.out[10000], q.in[10000], 1e-7);
    CHECK_NEAR (q.out[36000], q.in[36000], 1e-7);
    p.thresholdDb = -10.0;
    const auto t = runLimit (p, limiterStep());
    CHECK_NEAR (dbOf (t.out[36000]), -0.014, 0.005);
}

ROVOX_TEST (parity, realimit_gain_ramps_linearly_over_two_ms_ending_on_the_peak)
{
    fx::ReaLimitParams p;
    p.thresholdDb = -10.0;
    p.releaseDbPerSec = 6.0;
    const auto r = runLimit (p, limiterStep());
    const size_t step = 24000;
    const double end = 0.44598;                                                            // 0.99839 / (10^(7/20))

    for (long k = -100; k <= 0; k += 4)
    {
        const double want = k <= -96 ? 1.0 : 1.0 - (1.0 - end) * (double) (k + 96) / 96.0;
        CHECK_NEAR (r.gain ((size_t) ((long) step + k)), want, 2e-4);
    }

    CHECK_NEAR (r.gain (step - 92), 0.976916, 2e-4);                                       // measured
    CHECK_NEAR (r.gain (step - 48), 0.72299, 2e-4);
    CHECK_NEAR (r.gain (step), 0.44598, 2e-4);

    // -20 dB threshold: slope 0.9 / 96
    p.thresholdDb = -20.0;
    const auto s = runLimit (p, limiterStep());
    CHECK_NEAR (s.gain (step - 48), 0.570515, 2e-4);
}

ROVOX_TEST (parity, realimit_release_has_a_fast_linear_stage_and_a_slow_stage_in_db_per_second)
{
    const struct { double rate; double gr[8][2]; } cases[] = {
        { 6.0, { { 0, -7.0136 }, { 48, -7.0076 }, { 480, -6.9536 }, { 2400, -6.7136 }, { 9600, -5.8136 }, { 24000, -4.0136 }, { 43200, -1.6137 }, { 0, 0 } } },
        { 25.95, { { 0, -7.0132 }, { 48, -6.9872 }, { 480, -6.7537 }, { 2400, -5.7157 }, { 4800, -4.4183 }, { 9600, -1.8235 }, { 14400, 0.0 }, { 0, 0 } } },
        { 11.95, { { 0, -7.0134 }, { 480, -6.894 }, { 2400, -6.416 }, { 9600, -4.6235 }, { 19200, -2.2336 }, { 24000, -1.0386 }, { 43200, 0.0 }, { 0, 0 } } } };

    for (auto& c : cases)
    {
        fx::ReaLimitParams p;
        p.thresholdDb = -10.0;
        p.releaseDbPerSec = c.rate;
        const auto r = runLimit (p, limiterStep());

        for (auto& pt : c.gr)
            if (pt[0] > 0 || pt[1] != 0)
                CHECK_NEAR (dbOf (r.gain (48000 + (size_t) pt[0])), pt[1], 0.02);
    }

    // the fast stage after an isolated peak: back to 1 over 480 samples (10 ms), the slow stage then holds a fraction of a dB
    fx::ReaLimitParams p;
    p.thresholdDb = -10.0;
    p.releaseDbPerSec = 6.0;
    const auto r = runLimit (p, limiterImpulse());
    const size_t at = 120000;
    const struct { long k; double g; } pts[] = { { -100, 1.0 }, { -80, 0.885902 }, { -40, 0.600658 }, { 0, 0.315414 }, { 20, 0.343938 }, { 200, 0.600658 },
                                                 { 440, 0.942951 }, { 460, 0.968637 }, { 480, 0.968916 } };

    for (auto& pt : pts)
        CHECK_NEAR (r.gain ((size_t) ((long) at + pt.k)) , pt.g, 1.5e-3);

    p.releaseDbPerSec = 25.95;
    const auto r26 = runLimit (p, limiterImpulse());
    CHECK_NEAR (r26.gain (at + 400), 0.884328, 1.5e-3);
    CHECK_NEAR (r26.gain (at + 480), 0.888742, 1.5e-3);
    p.thresholdDb = -20.0;
    p.releaseDbPerSec = 15.0;
    const auto r20 = runLimit (p, limiterImpulse());
    CHECK_NEAR (r20.gain (at), 0.099743, 1.5e-3);
    CHECK_NEAR (r20.gain (at + 460), 0.926396, 1.5e-3);
}

ROVOX_TEST (parity, realimit_slow_stage_charges_with_the_length_of_a_burst)
{
    // measured residual gain reduction 10 ms after a burst that needs 7 dB (T = -10, 6 dB/s): the charge law is a fit (low confidence)
    const struct { double ms, gr; } bursts[] = { { 1, 0.29 }, { 5, 0.38 }, { 20, 0.72 }, { 100, 2.55 }, { 300, 6.95 } };

    for (auto& b : bursts)
    {
        auto x = dcSegments ({ { -30, 0.1 }, { -30, 0.9 } }, 48000.0);
        const size_t a = 4800, len = (size_t) std::lround (b.ms * 48.0);

        for (size_t i = a; i < a + len; ++i)
            x[i] = (float) std::pow (10.0, -3.0 / 20.0);

        fx::ReaLimitParams p;
        p.thresholdDb = -10.0;
        p.releaseDbPerSec = 6.0;
        const auto r = runLimit (p, x);
        CHECK_NEAR (-dbOf (r.gain (a + len + 480)), b.gr, 0.04);
    }

    // a release of "inf" has no slow stage: the gain is back after the fast stage
    fx::ReaLimitParams p;
    p.thresholdDb = -10.0;
    p.releaseDbPerSec = fx::ReaLimitParams::infiniteRelease;
    const auto r = runLimit (p, limiterStep());
    CHECK_NEAR (r.gain (48000 + 480), 1.0, 1e-6);
}

//------------------------------------------------------------------------------ translation
ROVOX_TEST (parity, reacomp_translation_reads_the_knobs_exactly_and_selects_the_reaper_mode)
{
    const json entry = { { "name", "ReaComp" },
                         { "params", json::array ({ prm (0, "Threshold", 0.249155231, "-6.0"), prm (1, "Ratio", 0.0303030312, "4.00"), prm (2, "Attack", 0.00599999987, "3.0"),
                                                    prm (3, "Release", 0.0199999996, "100"), prm (4, "Pre-comp", 0.0399999991, "10.0"), prm (5, "resvd", 0.0, "0"),
                                                    prm (6, "Lowpass", 0.0499749985, "1000"), prm (7, "Hipass", 1.0, "20000"), prm (8, "SignIn", 0.0, "0"), prm (9, "AudIn", 0.0, "0"),
                                                    prm (10, "Dry", 0.249155231, "-6.0"), prm (11, "Wet", 0.0, "-inf"), prm (12, "Filter Preview", 0.0, "0"),
                                                    prm (13, "RMS size", 0.00999499997, "10.0"), prm (14, "Knee", 0.5, "12.0"), prm (15, "Auto Make Up Gain", 1.0, "1"),
                                                    prm (16, "Auto Release", 0.0, "0"), prm (17, "Legacy Attack/Knee Options", 0.0, "0.00"),
                                                    prm (18, "Deprecated Broken Anti-Alias", 0.0, "0"), prm (19, "Multichannel Mode", 0.0, "0"), prm (20, "Metering Index", 0.0, "0"),
                                                    prm (21, "Bypass", 0.0, "normal"), prm (22, "Wet", 0.25, "25"), prm (23, "Delta", 1.0, "enabled") }) } };
    const auto t = translateReaperFx ("comp", parseReaperFxEntry (entry));
    CHECK_NEAR (translatedValue (t, "threshold"), 20.0 * std::log10 (2 * 0.249155231), 1e-9);
    CHECK_NEAR (translatedValue (t, "ratio"), 1.0 + 99.0 * 0.0303030312, 1e-9);
    CHECK_NEAR (translatedValue (t, "attack"), 500.0 * 0.00599999987, 1e-9);
    CHECK_NEAR (translatedValue (t, "release"), 100.0, 1e-3);
    CHECK_NEAR (translatedValue (t, "lookahead"), 250.0 * 0.0399999991, 1e-9);
    CHECK_NEAR (translatedValue (t, "lowpass"), 20000.0 * 0.0499749985, 1e-9);
    CHECK_NEAR (translatedValue (t, "hipass"), 20000.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "dry"), 20.0 * std::log10 (2 * 0.249155231), 1e-9);
    CHECK_NEAR (translatedValue (t, "wet"), -150.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "rms"), 1000.0 * 0.00999499997, 1e-9);
    CHECK_NEAR (translatedValue (t, "knee"), 12.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "auto_makeup"), 1.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "legacy_knee"), 0.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "reaper_compat"), 1.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "fx_wet"), 25.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "fx_delta"), 1.0, 1e-9);
    CHECK (t.untranslated.empty());

    const json inf = { { "name", "ReaComp" }, { "params", json::array ({ prm (1, "Ratio", 1.0, "inf") }) } };
    CHECK_NEAR (translatedValue (translateReaperFx ("comp", parseReaperFxEntry (inf)), "ratio"), 100.0, 1e-9);
}

ROVOX_TEST (parity, reagate_and_realimit_translation_read_the_knobs_exactly)
{
    const json gate = { { "name", "ReaGate" },
                        { "params", json::array ({ prm (0, "Threshold", 0.249155231, "-6.0"), prm (1, "Attack", 0.00200000009, "1"), prm (2, "Release", 0.0199999996, "100"),
                                                   prm (3, "Pre-open", 0.0399999991, "10"), prm (4, "Hold", 0.0500000007, "50"), prm (5, "Lowpass", 1.0, "20000"),
                                                   prm (6, "Hipass", 0.0, "0"), prm (7, "SignIn", 0.0, "0"), prm (8, "AudIn", 0.0, "0"), prm (9, "Dry", 0.249155231, "-6.0"),
                                                   prm (10, "Wet", 0.5, "+0.0"), prm (11, "Noise level", 0.0, "-inf"), prm (12, "Hysteresis", 0.249155231, "-6.0"),
                                                   prm (13, "Preview Filter", 0.0, "0"), prm (14, "RMS size", 0.0, "0"), prm (15, "Send MIDI", 0.0, "0"),
                                                   prm (16, "Midi Note", 0.5, "64"), prm (17, "Midi Channel", 0.0, "1"), prm (18, "Invert Wet", 1.0, "enabled"),
                                                   prm (19, "Multichannel Mode", 0.0, "0"), prm (20, "Metering Index", 0.0, "0"),
                                                   prm (21, "Bypass", 0.0, "normal"), prm (22, "Wet", 1.0, "100"), prm (23, "Delta", 0.0, "normal") }) } };
    const auto g = translateReaperFx ("gate", parseReaperFxEntry (gate));
    CHECK_NEAR (translatedValue (g, "threshold"), 20.0 * std::log10 (2 * 0.249155231), 1e-9);
    CHECK_NEAR (translatedValue (g, "attack"), 1.0, 1e-9);                // ReaGate acts on the whole-ms text, not the knob position
    CHECK_NEAR (translatedValue (g, "release"), 100.0, 1e-3);
    CHECK_NEAR (translatedValue (g, "pre_open"), 10.0, 1e-9);
    CHECK_NEAR (translatedValue (g, "hold"), 50.0, 1e-3);
    CHECK_NEAR (translatedValue (g, "dry"), 20.0 * std::log10 (2 * 0.249155231), 1e-9);
    CHECK_NEAR (translatedValue (g, "wet"), 0.0, 1e-9);
    CHECK_NEAR (translatedValue (g, "rea_hysteresis"), 20.0 * std::log10 (2 * 0.249155231), 1e-9);
    CHECK_NEAR (translatedValue (g, "invert_wet"), 1.0, 1e-9);
    CHECK_NEAR (translatedValue (g, "reaper_compat"), 1.0, 1e-9);
    CHECK (g.untranslated.empty());

    const json lim = { { "name", "ReaLimit" },
                       { "params", json::array ({ prm (0, "Threshold", 0.5, "-24.00"), prm (1, "Ceiling", 0.99, "-0.24"), prm (2, "Release", 0.5, "12.0"),
                                                  prm (3, "Bypass", 0.0, "normal"), prm (4, "Wet", 1.0, "100"), prm (5, "Delta", 0.0, "normal") }) } };
    const auto l = translateReaperFx ("limit", parseReaperFxEntry (lim));
    CHECK_NEAR (translatedValue (l, "threshold"), -60.0 + 72.0 * 0.5, 1e-9);
    CHECK_NEAR (translatedValue (l, "ceiling"), -24.0 + 24.0 * 0.99, 1e-9);
    CHECK_NEAR (translatedValue (l, "release_rate"), 6.0 - 20.0 * std::log10 (0.5), 1e-9);
    CHECK_NEAR (translatedValue (l, "reaper_compat"), 1.0, 1e-9);
    CHECK (l.untranslated.empty());

    const json infinite = { { "name", "ReaLimit" }, { "params", json::array ({ prm (2, "Release", 0.0, "inf") }) } };
    CHECK_NEAR (translatedValue (translateReaperFx ("limit", parseReaperFxEntry (infinite)), "release_rate"), 1000.0, 1e-9);
}

//------------------------------------------------------------------------------ the hidden fade stage
ROVOX_TEST (parity, the_fade_stage_is_hidden_from_fx_lists_and_indices_but_still_applies_its_fade)
{
    test::Env env;
    const auto dir = test::makeTempDir ("parityhide");
    test::writeWav (dir / "item.wav", { std::vector<float> (48000, 0.5f) }, 48000, 24);
    std::ofstream (dir / "t.RPP") << fadeRpp ("      FADEIN 4 0.25 0 4 0 1 1 1 0 0 1 0 0\n      FADEOUT 0 0.25 0 0 0 0 0 1 0 0 1 0 0");
    const auto id = env.newProject ("hide");
    REQUIRE (env.run (id, "import.rpp", { { "path", (dir / "t.RPP").string() } }).ok);
    const auto track = env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();

    const auto typesOf = [&]
    {
        std::vector<std::string> types;
        const auto list = env.ok (id, "fx.list", { { "target", track } });

        for (auto& f : list["fx"])
        {
            CHECK (f["index"].get<size_t>() == types.size());       // indices count only what users see
            types.push_back (f["type"].get<std::string>());
        }

        return types;
    };

    auto types = typesOf();
    const size_t before = types.size();
    CHECK (std::find (types.begin(), types.end(), "rovoxfades") == types.end());

    const auto eq = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "rovox:eq" } } }, { "index", 0 } })["plugin_id"].get<std::string>();
    const auto comp = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "rovox:comp" } } }, { "index", 0 } })["plugin_id"].get<std::string>();
    types = typesOf();

    if (types.size() != before + 2)
    {
        std::cerr << "    chain before " << before << ", after:";

        for (auto& t : types)
            std::cerr << " " << t;

        std::cerr << "\n";
    }

    REQUIRE (types.size() == before + 2);
    CHECK (types[0] == "rovoxcomp");                // index 0 is the first FX the user sees
    CHECK (types[1] == "rovoxeq");
    const auto listed = env.ok (id, "fx.list", { { "target", track } });
    CHECK (listed["fx"][0]["plugin_id"] == comp);
    CHECK (listed["fx"][1]["plugin_id"] == eq);
    env.ok (id, "fx.move", { { "plugin_id", comp }, { "to_index", 1 } });
    types = typesOf();
    CHECK (types[0] == "rovoxeq");
    CHECK (types[1] == "rovoxcomp");

    const auto tracks = env.ok (id, "track.list");

    for (auto& t : tracks["tracks"])
        for (auto& pl : t["plugins"])
            CHECK (pl["type"] != "rovoxfades");

    const auto project = env.ok (id, "project.get", { { "include", { "fx" } } });

    for (auto& pl : project["fx"][track])
        CHECK (pl["type"] != "rovoxfades");

    // the stage is still there, first in the chain: the fade-in renders (the EQ is flat at its defaults, the compressor is switched off)
    env.ok (id, "fx.update", { { "plugin_id", comp }, { "enabled", false } });
    RenderRequest rq;
    rq.tailS = 0.0;
    auto rr = renderProject (env.project (id), rq);
    REQUIRE (rr.ok());
    std::vector<std::vector<float>> ch;
    double rate = 0.0;
    REQUIRE (test::readWav (env.project (id).dir() / rr.value()["file"]["path"].get<std::string>(), ch, rate));
    REQUIRE (! ch.empty());
    CHECK (std::abs (ch[0][0]) < 1.0e-6f);
    CHECK_NEAR (ch[0][(size_t) (0.75 * 0.25 * rate)], 0.5 * std::pow (0.75, 4.0), 5.0e-3);       // shape 4 (curve -1): t^4 at three quarters of the 0.25 s fade
    CHECK_NEAR (ch[0][30000], 0.5, 1.0e-2);
}
