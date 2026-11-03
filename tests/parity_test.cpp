// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER parity of the item fades, the ReaEQ band types and ReaDelay. The expectations are computed in this file from the formulas of the
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
