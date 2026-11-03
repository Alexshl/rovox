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

#include <array>
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
                                        fx::BandType::AllPass, fx::BandType::Notch, fx::BandType::BandPass, fx::BandType::Peak, fx::BandType::PeakAlt,
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

// ReaEQ v3: the gain is an amplitude, -inf / very low gains switch the filter type
Iir v3Band (int code, double f0, double amp, double bw, double fs)
{
    const double w0 = 2 * pi * f0 / fs, c = std::cos (w0), s = std::sin (w0);
    const double alpha = s * std::sinh (std::log (2.0) / 2 * bw * w0 / s);
    const auto set = [] (double b0, double b1, double b2, double a0, double a1, double a2) -> Iir
    {
        return { { b0 / a0, b1 / a0, b2 / a0 }, { 1, a1 / a0, a2 / a0 } };
    };

    if (code == 0 && amp < 0.01)    return set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    if (code == 1 && amp < 0.01)    return set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    if (code == 8 && amp < 0.001)   return set (1, -2 * c, 1, 1 + alpha, -2 * c, 1 - alpha);

    if (code == 2 || code == 9)
        amp = std::max (amp, 3.0e-5);

    return specBand (code, f0, 20.0 * std::log10 (amp), bw, fs);
}

std::vector<float> eqSeriesImpulse (const std::vector<std::array<double, 5>>& bands, size_t n, double fs)
{
    // band = { code, freq, amp, bw, enabled }
    fx::EqParams p;
    p.reaperCompat = true;

    for (size_t i = 0; i < 8; ++i)
        p.bands[i].enabled = false;

    for (size_t i = 0; i < bands.size(); ++i)
        p.bands[i] = { bands[i][4] != 0.0, typeOfCode ((int) bands[i][0]), bands[i][1], bands[i][2] <= 0.0 ? -150.0 : 20.0 * std::log10 (bands[i][2]), bands[i][3] };

    return eqImpulse (p, n, fs);
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

        // the 48 kHz media is resampled by a sinc converter: the edges of the item ring at about -86 dB (2.5e-5 of the 0.5 DC level)
        if (worst >= 5.0e-5)
            std::cerr << "    worst error " << worst << " at " << worstAt << " of " << out.size() << " (L " << L << "): got " << out[worstAt] << "\n";

        CHECK (worst < 5.0e-5);
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

ROVOX_TEST (parity, reaeq_v3_very_low_gains_switch_the_band_filters)
{
    const double fs = 48000.0;
    double worst = 0.0;
    struct Case { int code; double amp; };
    const Case cases[] = { { 0, 0.0 }, { 0, 0.00998 }, { 0, 0.01 }, { 0, 0.5 }, { 1, 0.0 }, { 1, 0.00998 }, { 1, 0.01 }, { 8, 0.0 }, { 8, 0.000999 }, { 8, 0.001 },
                           { 9, 0.0 }, { 9, 1.0e-5 }, { 9, 0.001 }, { 2, 0.0 }, { 2, 1.0e-5 }, { 2, 0.01 }, { 3, 0.0 }, { 4, 0.0 }, { 5, 0.0 }, { 6, 0.0 },
                           { 7, 0.0 }, { 10, 0.0 } };

    for (auto& c : cases)
        for (double f0 : { 100.0, 768.3, 8132.8 })
            for (double bw : { 0.8, 2.0 })
            {
                const auto got = eqSeriesImpulse ({ { (double) c.code, f0, c.amp, bw, 1.0 } }, 2048, fs);
                const auto want = iirImpulse (v3Band (c.code, f0, c.amp, bw, fs), 2048);

                for (size_t i = 0; i < want.size(); ++i)
                    worst = std::max (worst, std::abs ((double) got[i] - want[i]));
            }

    CHECK (worst < 3.0e-6);
}

ROVOX_TEST (parity, reaeq_v3_minus_inf_low_shelf_is_a_high_pass_with_the_band_q_and_dc_gain_zero)
{
    const auto ir = eqSeriesImpulse ({ { 0, 768.315, 0.0, 0.8, 1.0 } }, 48000, 48000.0);
    double dc = 0.0;

    for (float v : ir)
        dc += v;

    CHECK_NEAR (dc, 0.0, 1.0e-4);
    CHECK_NEAR (ir[0], 0.970074296, 2.0e-6);      // measured first sample (BW 0.8)
}

ROVOX_TEST (parity, reaeq_v3_mykola_instances_match_the_measured_impulse_samples)
{
    // the three ReaEQ instances of the user's project (bands as stored: type, freq, amplitude, BW); measured first impulse samples at 48 kHz
    struct Inst { std::vector<std::array<double, 5>> bands; std::vector<double> ir; };
    const Inst insts[] = {
        { { { 0, 768.3150636133457, 0.0, 0.8, 1 }, { 8, 300.0, 1.0, 2.0, 1 }, { 8, 1000.0, 1.0, 2.0, 1 }, { 1, 5000.0, 1.0, 0.8, 1 }, { 4, 100.0, 1.0, 2.0, 0 } },
          { 0.970074296, -0.062827587, -0.068293028, -0.072786979, -0.076318651, -0.078906193, -0.080576047, -0.081362203 } },
        { { { 0, 202.3323704927588, 0.5199999809265137, 0.8, 1 }, { 8, 580.9411298908843, 0.8799999952316284, 2.0, 1 }, { 8, 1000.0, 1.0, 2.0, 1 },
            { 1, 5000.0, 1.0, 0.8, 1 }, { 4, 100.0, 1.0, 2.0, 0 } },
          { 0.987571657, -0.023962529, -0.022239244, -0.020642865, -0.01916684, -0.017804692, -0.016550045, -0.015396655 } },
        { { { 0, 8132.787581251436, 0.0, 0.8, 1 }, { 8, 580.9411298908843, 0.8799999952316284, 2.0, 1 }, { 8, 1000.0, 1.0, 2.0, 1 },
            { 1, 5000.0, 1.0, 0.8, 1 }, { 4, 100.0, 1.0, 2.0, 0 } },
          { 0.566827774, -0.71846664, -0.265179098, 0.188428, 0.282945424, 0.109590158, -0.070447378, -0.111480646 } } };

    for (auto& inst : insts)
    {
        const auto got = eqSeriesImpulse (inst.bands, 64, 48000.0);

        for (size_t i = 0; i < inst.ir.size(); ++i)
            CHECK_NEAR (got[i], inst.ir[i], 2.0e-6);
    }
}

ROVOX_TEST (parity, reaeq_v3_native_eq_keeps_its_gain_range)
{
    fx::EqParams a, b;

    for (auto* p : { &a, &b })
        for (size_t i = 1; i < 8; ++i)
            p->bands[i].enabled = false;

    a.bands[0] = { true, fx::BandType::Peak, 1000.0, -100.0, 1.0 };
    b.bands[0] = { true, fx::BandType::Peak, 1000.0, -30.0, 1.0 };
    const auto x = eqImpulse (a, 256, 48000.0), y = eqImpulse (b, 256, 48000.0);

    for (size_t i = 0; i < x.size(); ++i)
        CHECK_NEAR (x[i], y[i], 1e-9);
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

ROVOX_TEST (parity, reaeq_v3_translation_reads_minus_inf_as_zero_amplitude_and_band_alt_as_its_own_type)
{
    const json entry = { { "name", "ReaEQ" },
                         { "config", { { "BANDTYPE0", "0" }, { "BANDENABLED0", "1" }, { "BANDTYPE1", "9" }, { "BANDENABLED1", "1" } } },
                         { "params", json::array ({ prm (0, "Freq-Low Shelf", 0.434, "768.3"), prm (1, "Gain-Low Shelf", 0.0, "-inf"), prm (2, "BW-Low Shelf", 0.2, "0.80"),
                                                    prm (3, "Freq-Band 2", 0.5, "1160.5"), prm (4, "Gain-Band 2", 1.0e-6, "-114.0"), prm (5, "BW-Band 2", 0.5, "2.00") }) } };
    const auto t = translateReaperFx ("eq", parseReaperFxEntry (entry));
    CHECK_NEAR (translatedValue (t, "b1_gain"), -150.0, 1e-9);
    CHECK_NEAR (translatedValue (t, "b2_type"), 10.0, 1e-9);        // code 9 = Band (alt)
    CHECK_NEAR (translatedValue (t, "b2_gain"), 20.0 * std::log10 (2.0e-6), 1e-9);
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

ROVOX_TEST (parity, reagate_hold_and_attack_add_up_and_the_release_starts_after_them)
{
    const auto a10 = runGate (gateParams (10.0, 100.0));
    CHECK_NEAR (a10.gain (1, 0), 0.002193, 1e-6);                   // 1 / 456
    CHECK_NEAR (a10.gain (1, 100), 0.221491, 1e-6);
    CHECK_NEAR (a10.gain (1, 455), 1.0, 1e-6);
    CHECK_NEAR (a10.gain (2, 454), 1.0, 1e-9);                      // hold floor (456) - 1 samples (the attack time is added to the zero hold)
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

namespace
{
/** Timings of a gate run on a DC burst (-40 dBFS 0.3 s, -10 dBFS `burstMs`, -40 dBFS 0.8 s); all relative to the onset or the drop of the burst. */
struct BurstGate
{
    long firstOpen = 0, firstFull = -1, lastFull = -1, firstZero = -1;
};

BurstGate runBurst (const fx::ReaGateParams& p, double burstMs, double fs)
{
    std::vector<size_t> starts;
    const auto in = dcSegments ({ { -40, 0.3 }, { -10, burstMs / 1000.0 }, { -40, 0.8 } }, fs, &starts);
    fx::ReaGate g;
    g.prepare (fs);
    g.setParams (p);
    const auto out = runMono (g, in, g.latencySamples());
    const long onset = (long) starts[1], drop = (long) starts[2];
    BurstGate r;

    for (long i = 0; i < drop; ++i)
        if (out[(size_t) i] != 0.0f)
        {
            r.firstOpen = i - onset;
            break;
        }

    for (long i = onset; i < (long) in.size(); ++i)
    {
        const bool full = (double) out[(size_t) i] / (double) in[(size_t) i] >= 1.0 - 1.0e-6;

        if (full && r.firstFull < 0 && i < drop)
            r.firstFull = i - onset;

        if (full && i >= drop)
            r.lastFull = i - drop;

        if (i >= drop && r.firstZero < 0 && std::abs ((double) out[(size_t) i] / (double) in[(size_t) i]) < 1.0e-9)
            r.firstZero = i - drop;
    }

    return r;
}

struct GateStairCase { double hy, attack, release, hold; double g[18][4]; };
const GateStairCase gateStairCases[] = {
    { -6.0, 0.0, 0.0, 0.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
    } },
    { -6.0, 3.0, 50.0, 5.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.534933 },
        { 0.543266, 0.551599, 0.559933, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
    } },
    { -3.0, 0.0, 0.0, 0.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
    } },
    { -0.0, 0.0, 0.0, 0.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
    } },
    { -0.0, 3.0, 50.0, 5.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.534933 },
        { 0.543266, 0.551599, 0.559933, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
    } },
    { 3.0, 0.0, 0.0, 0.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 1.0, 0.0, 1.0 },
        { 0.0, 1.0, 0.0, 1.0 },
        { 0.0, 1.0, 0.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
    } },
    { 3.0, 3.0, 50.0, 5.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 0.785354 },
        { 0.784933, 0.784512, 0.784091, 0.0 },
        { 0.008333, 0.016667, 0.025, 0.951599 },
        { 0.951178, 0.950758, 0.950337, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.90867 },
        { 0.908249, 0.907828, 0.907407, 0.864478 },
        { 0.864057, 0.863636, 0.863215, 0.803872 },
        { 0.803451, 0.80303, 0.802609, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.534933 },
        { 0.543266, 0.551599, 0.559933, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
    } },
    { 6.0, 0.0, 0.0, 0.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 1.0, 0.0 },
        { 1.0, 0.0, 1.0, 0.0 },
        { 1.0, 0.0, 1.0, 0.0 },
        { 1.0, 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 0.0, 0.0, 0.0, 0.0 },
    } },
    { 6.0, 3.0, 50.0, 5.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 0.138468 },
        { 0.138047, 0.137626, 0.137205, 0.0 },
        { 0.008333, 0.016667, 0.025, 0.727694 },
        { 0.727273, 0.726852, 0.726431, 0.0 },
        { 0.008333, 0.016667, 0.025, 0.861953 },
        { 0.861532, 0.861111, 0.86069, 0.430976 },
        { 0.430556, 0.430135, 0.429714, 0.297138 },
        { 0.296717, 0.296296, 0.295875, 0.427609 },
        { 0.427189, 0.426768, 0.426347, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.008333, 0.016667, 0.025, 1.0 },
        { 1.0, 1.0, 1.0, 0.534933 },
        { 0.543266, 0.551599, 0.559933, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
    } },
    { 0.0, 10.0, 100.0, 0.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.002193, 0.004386, 0.006579, 1.0 },
        { 1.0, 1.0, 1.0, 0.090243 },
        { 0.092436, 0.094629, 0.096822, 1.0 },
        { 1.0, 1.0, 1.0, 0.090243 },
        { 0.092436, 0.094629, 0.096822, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 0.090243 },
        { 0.090034, 0.089824, 0.089615, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.002193, 0.004386, 0.006579, 1.0 },
        { 1.0, 1.0, 1.0, 0.79376 },
        { 0.795953, 0.798146, 0.800339, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
    } },
    { 0.0, 30.0, 100.0, 0.0, {
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.000706, 0.001412, 0.002119, 1.0 },
        { 1.0, 1.0, 1.0, 0.291248 },
        { 0.291954, 0.29266, 0.293367, 1.0 },
        { 1.0, 1.0, 1.0, 0.291248 },
        { 0.291954, 0.29266, 0.293367, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 1.0 },
        { 1.0, 1.0, 1.0, 0.291248 },
        { 0.291039, 0.290829, 0.29062, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.0, 0.0, 0.0, 0.0 },
        { 0.000706, 0.001412, 0.002119, 1.0 },
        { 1.0, 1.0, 1.0, 0.994765 },
        { 0.995472, 0.996178, 0.996884, 1.0 },
        { 1.0, 1.0, 1.0, 0.0 },
    } },
};
} // namespace

ROVOX_TEST (parity, reagate_v2_attack_is_added_to_the_hold_and_the_release_follows)
{
    // attack, hold, release (ms) -> last full-gain sample after the drop, first zero sample after the drop (measured: gate2_* DC bursts, 48 kHz)
    const struct { double a, h, r; long lastFull, zero; } rows[] = {
        { 0, 0, 100, -1, 4776 },    { 0, 5, 100, 215, 4992 },     { 0, 20, 100, 935, 5712 },     { 0, 50, 100, 2375, 7152 },
        { 1, 0, 100, 22, 4799 },    { 1, 5, 100, 238, 5015 },     { 1, 20, 100, 958, 5735 },     { 1, 50, 100, 2398, 7175 },
        { 3, 0, 100, 118, 4895 },   { 3, 5, 100, 334, 5111 },     { 3, 20, 100, 1054, 5831 },    { 3, 50, 100, 2494, 7271 },
        { 10, 0, 100, 454, 5231 },  { 10, 5, 100, 670, 5447 },    { 10, 20, 100, 1390, 6167 },   { 10, 50, 100, 2830, 7607 },
        { 30, 0, 100, 1414, 6191 }, { 30, 20, 100, 2350, 7127 },  { 30, 50, 100, 3790, 8567 },
        { 3, 20, 1, 1054, 1079 },   { 3, 20, 10, 1054, 1511 },    { 3, 20, 50, 1054, 3431 },     { 3, 20, 300, 1054, 15431 } };

    for (auto& row : rows)
        for (double burst : { 2.0, 200.0 })
        {
            auto p = gateParams (row.a, row.r);
            p.holdMs = row.h;
            const auto b = runBurst (p, burst, 48000.0);
            CHECK (b.firstOpen == 0);
            CHECK (b.lastFull == row.lastFull);
            CHECK (std::abs (b.firstZero - row.zero) <= 1);      // the last step of the ramp leaves a rounding residue (REAPER: exactly zero one sample later)
        }

    // the attack ramp starts at the onset: g[n] = (n + 1) / Na, and an attack that has started completes during a short burst
    for (double a : { 3.0, 10.0, 30.0 })
    {
        const double Na = a * 48.0 - 24.0;
        const auto r = runGate (gateParams (a, 100.0));

        for (long n : { 0L, 1L, 2L, 10L, 100L })
            CHECK_NEAR (r.gain (1, n), std::min (1.0, (double) (n + 1) / Na), 2e-6);
    }
}

ROVOX_TEST (parity, reagate_v2_times_are_milliseconds_at_every_sample_rate)
{
    // first full-gain sample after the onset, first sample below full after the drop, first zero after the drop, pre-open start (measured: sr*_gate_*)
    const struct { double fs; long full1, below1, zero1, full10, below10, zero10, preOpen; } rows[] = {
        { 44100.0, 22, 21, 2203, 418, 1276, 5663, -418 },
        { 48000.0, 23, 23, 2399, 455, 1391, 6167, -456 },
        { 96000.0, 47, 47, 4799, 911, 2783, 12335, -912 } };

    for (auto& row : rows)
    {
        const auto a1 = runBurst (gateParams (1.0, 50.0), 200.0, row.fs);
        CHECK (a1.firstFull == row.full1);
        CHECK (a1.lastFull + 1 == row.below1);
        CHECK (std::abs (a1.firstZero - row.zero1) <= 1);

        auto p = gateParams (10.0, 100.0);
        p.holdMs = 20.0;
        const auto a10 = runBurst (p, 200.0, row.fs);
        CHECK (a10.firstFull == row.full10);
        CHECK (a10.lastFull + 1 == row.below10);
        CHECK (std::abs (a10.firstZero - row.zero10) <= 1);

        p = gateParams (0.0, 0.0);
        p.preOpenMs = 10.0;
        const auto pre = runBurst (p, 200.0, row.fs);
        CHECK (pre.firstOpen == row.preOpen);
        CHECK (pre.lastFull == -1);                                 // closing is not advanced
    }
}

ROVOX_TEST (parity, reagate_v2_threshold_moves_with_the_gain_hysteresis_staircase_and_retrigger)
{
    const double levels[] = { -40, -19, -40, -17.5, -40, -15, -17.5, -18.5, -19.5, -21, -23, -25, -27, -40, -10, -40, -10, -40 };
    const double lengthMs[] = { 200, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 200, 100, 30, 20, 300 };
    std::vector<Seg> segs;

    for (size_t s = 0; s < 18; ++s)
        segs.push_back ({ levels[s], lengthMs[s] / 1000.0 });

    // first three samples and the last sample of every segment; a positive hysteresis chatters (the gate flips every sample)
    for (auto& c : gateStairCases)
    {
        auto p = gateParams (c.attack, c.release);
        p.holdMs = c.hold;
        p.hysteresisDb = c.hy != 0.0 ? c.hy - 0.05 : 0.0;       // the knob shows -0.05 dB less than its text, as the threshold does
        const auto r = runGate (p, segs);

        for (size_t s = 0; s < 18; ++s)
        {
            const long last = std::lround (lengthMs[s] * 48.0) - 1;

            for (int k = 0; k < 4; ++k)
                CHECK_NEAR (r.gain (s, k < 3 ? k : last), c.g[s][k], 2e-6);
        }
    }
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

ROVOX_TEST (parity, reacomp_v2_legacy_option_selects_the_knee_and_where_the_attack_starts)
{
    // DC -40 -> -6 dBFS, threshold -30.05, ratio inf: gain (dB) at 0.5 / 1 / 2 / 5 / 10 ms after the onset (measured: comp6_*_leg*)
    const struct { double attack, release; bool fromEnvelope; long n[5]; double db[5]; } rows[] = {
        { 50, 200, false, { 24, 48, 96, 240, 480 }, { -1.251333, -2.293592, -4.040546, -7.706919, -11.399879 } },
        { 50, 200, true, { 24, 48, 96, 240, 480 }, { 0.0, 0.0, 0.0, -5.162718, -9.986667 } },
        { 10, 50, false, { 24, 48, 96, 240, 480 }, { -4.919338, -7.806696, -11.470409, -16.80075, -20.407786 } },
        { 10, 50, true, { 24, 48, 96, 240, 480 }, { -0.943637, -5.3023, -10.073399, -16.268025, -20.198926 } },
        { 1, 50, true, { 24, 48, 96, 240, 480 }, { -16.531547, -20.313135, -22.865254, -23.994038, -24.047252 } } };

    for (auto& row : rows)
    {
        auto p = compParams (-30.05, row.attack, row.release);
        p.attackFromEnvelope = row.fromEnvelope;
        const auto r = runComp (p);
        for (int k = 0; k < 5; ++k)
            CHECK_NEAR (r.up (row.n[k]), row.db[k], 0.15);
    }

    // with a soft knee the attack starts from the start of the knee (threshold - W), default knee 6 dB, attack 10 ms (measured: comp6_a10_r50_k6_leg0.25;
    // the legacy knee has only a 12 dB table, its shape at 6 dB is scaled from that and not checked)
    auto kp = compParams (-30.05, 10.0, 50.0);
    kp.kneeDb = 6.0;
    const auto kr = runComp (kp);
    CHECK_NEAR (kr.up (0), -0.184462, 0.1);
    CHECK_NEAR (kr.up (24), -3.655902, 0.15);
    CHECK_NEAR (kr.up (48), -6.060116, 0.15);

    // the release is the same for both: 50 ms release after the drop (measured: gain_db_after_drop_ms, all four classes)
    auto p = compParams (-30.05, 10.0, 50.0);
    p.attackFromEnvelope = true;
    const auto r = runComp (p);
    CHECK_NEAR (r.down (48 * 10), -22.328186, 0.2);
    CHECK_NEAR (r.down (48 * 50), -15.568775, 0.2);

    // the legacy knee (K_12 (0) = 2.410 dB) goes with floor (4 n) even, the default knee with odd; here only the knee flag
    fx::ReaCompParams k;
    k.ratio = 100.0;
    k.kneeDb = 12.0;
    CHECK_NEAR (-fx::ReaComp::staticGainDb (k, 0.0), 4.0637, 0.05);
    k.legacyKnee = true;
    CHECK_NEAR (-fx::ReaComp::staticGainDb (k, 0.0), 2.4099, 0.05);
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
    CHECK_NEAR (dbOf (r.out[36000]), -6.009, 0.01);                                       // a -3 dBFS DC would be +7: -6.01 dBFS (measured)
    double peak = 0.0;

    for (float v : r.out)
        peak = std::max (peak, (double) std::abs (v));

    CHECK (dbOf (peak) < -6.0);
    CHECK (dbOf (peak) > -6.1);

    // below 0 dBFS nothing happens, a boosted peak settles 0.009 dB below the ceiling (the 0.014 dB of the first measurement was the gain model)
    p.thresholdDb = 0.0;
    p.ceilingDb = 0.0;
    const auto q = runLimit (p, limiterStep());
    CHECK_NEAR (q.out[10000], q.in[10000], 1e-7);
    CHECK_NEAR (q.out[36000], q.in[36000], 1e-7);
    p.thresholdDb = -10.0;
    const auto t = runLimit (p, limiterStep());
    CHECK_NEAR (dbOf (t.out[36000]), -0.009, 0.005);
}

ROVOX_TEST (parity, realimit_gain_ramps_linearly_over_two_ms_ending_on_the_peak)
{
    fx::ReaLimitParams p;
    p.thresholdDb = -10.0;
    p.releaseDbPerSec = 6.0;
    const auto r = runLimit (p, limiterStep());
    const size_t step = 24000;
    const double end = std::pow (10.0, -0.009 / 20.0) / std::pow (10.0, 7.0 / 20.0);

    for (long k = -100; k <= 0; k += 4)
    {
        const double want = k <= -96 ? 1.0 : 1.0 - (1.0 - end) * (double) (k + 96) / 96.0;
        CHECK_NEAR (r.gain ((size_t) ((long) step + k)), want, 2e-4);
    }

    CHECK_NEAR (r.gain (step - 92), 1.0 - (1.0 - end) * 4.0 / 96.0, 2e-4);
    CHECK_NEAR (r.gain (step - 48), 1.0 - (1.0 - end) * 0.5, 2e-4);
    CHECK_NEAR (r.gain (step), end, 2e-4);

    // -20 dB threshold: slope 0.9 / 96
    p.thresholdDb = -20.0;
    const auto s = runLimit (p, limiterStep());
    CHECK_NEAR (s.gain (step - 48), 1.0 - (1.0 - std::pow (10.0, -0.009 / 20.0) / std::pow (10.0, 17.0 / 20.0)) * 0.5, 2e-4);
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

namespace
{
struct LimStep { const char* rate; double over, ms; std::vector<std::pair<double, double>> gr; };
const LimStep limSteps[] = {
    { "6", 3, 10, { { 0, -3.006197 }, { 1, -2.653505 }, { 5, -1.370581 }, { 10, -0.471877 }, { 20, -0.411878 }, { 50, -0.23188 }, { 100, -2e-06 }, { 200, -2e-06 }, { 500, -2e-06 } } },
    { "6", 3, 100, { { 0, -3.006197 }, { 1, -2.653505 }, { 5, -2.462502 }, { 10, -2.432503 }, { 20, -2.372503 }, { 50, -2.192506 }, { 100, -1.892511 }, { 200, -1.292519 }, { 500, -2e-06 } } },
    { "6", 9, 300, { { 0, -8.980776 }, { 1, -7.531415 }, { 5, -7.253752 }, { 10, -7.223753 }, { 20, -7.163754 }, { 50, -6.983756 }, { 100, -6.683761 }, { 200, -6.083769 }, { 500, -4.283795 } } },
    { "6", 9, 1000, { { 0, -9.013574 }, { 1, -9.007574 }, { 5, -8.983574 }, { 10, -8.953575 }, { 20, -8.893576 }, { 50, -8.713578 }, { 100, -8.413583 }, { 200, -7.813592 }, { 500, -6.013617 } } },
    { "20", 3, 30, { { 0, -3.006197 }, { 1, -2.653505 }, { 5, -2.558178 }, { 10, -2.458432 }, { 20, -2.258942 }, { 50, -1.66047 }, { 100, -0.663018 }, { 200, -2e-06 }, { 500, -2e-06 } } },
    { "20", 9, 100, { { 0, -8.980776 }, { 1, -8.172021 }, { 5, -8.092225 }, { 10, -7.99248 }, { 20, -7.79299 }, { 50, -7.194518 }, { 100, -6.197066 }, { 200, -4.202161 }, { 500, -2e-06 } } },
    { "20", 9, 1000, { { 0, -9.013283 }, { 1, -8.993334 }, { 5, -8.913538 }, { 10, -8.813793 }, { 20, -8.614303 }, { 50, -8.015831 }, { 100, -7.018378 }, { 200, -5.023474 }, { 500, -2e-06 } } },
    { "inf", 3, 30, { { 0, -3.010576 }, { 1, -2.860629 }, { 5, -2.260845 }, { 10, -1.511115 }, { 20, -0.011655 }, { 50, -2e-06 }, { 100, -2e-06 }, { 200, -2e-06 }, { 500, -2e-06 } } },
    { "inf", 9, 100, { { 0, -9.010575 }, { 1, -8.860629 }, { 5, -8.260845 }, { 10, -7.511115 }, { 20, -6.011654 }, { 50, -1.513273 }, { 100, -2e-06 }, { 200, -2e-06 }, { 500, -2e-06 } } },
    { "inf", 9, 1000, { { 0, -9.010575 }, { 1, -8.860629 }, { 5, -8.260845 }, { 10, -7.511115 }, { 20, -6.011654 }, { 50, -1.513273 }, { 100, -2e-06 }, { 200, -2e-06 }, { 500, -2e-06 } } },
};
struct LimTrain { const char* rate; double period, duty; std::vector<double> gr; };
const LimTrain limTrains[] = {
    { "6", 100, 0.25, { -0.471758, -0.911764, -1.328644, -1.7249, -2.10178, -2.461161, -2.804292, -3.133048, -3.448054, -3.75056, -4.041191, -4.320572, -4.589953, -4.850585, -5.101841, -5.344972, -5.579978, -5.807485, -6.028116, -6.241872 } },
    { "6", 100, 0.5, { -1.19488, -2.323634, -3.401763, -4.436766, -5.435521, -6.4024, -7.341779, -8.257408, -8.731703, -8.731578, -8.731578, -8.731578, -8.731578, -8.731578, -8.731578, -8.731578, -8.731578, -8.731578, -8.731578, -8.731578 } },
    { "6", 20, 0.5, { -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -8.971575 } },
    { "inf", 100, 0.25, { -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06, -2e-06 } },
    { "inf", 100, 0.5, { -1.963111, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234, -1.966234 } },
};
struct LimPair { const char* rate; bool impulse; double over, gapMs; std::vector<double> gr; };
const LimPair limPairs[] = {
    { "6", false, 9, 1, { -9.013699, -9.013699, -9.013699 } },
    { "6", false, 9, 8, { -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699 } },
    { "6", false, 9, 12, { -8.980776, -8.2259, -7.531415, -6.88837, -6.318633, -6.435264, -6.553483, -6.673333, -6.794861, -6.918112, -7.043138, -7.16999, -7.298721, -7.42939, -7.562054, -7.696776, -7.83362, -7.972656, -8.113952, -8.257586, -8.403634, -8.552181, -8.703312, -8.857119, -9.013699 } },
    { "6", false, 3, 3, { -8.995745, -8.58612, -8.213602, -7.873472, -7.561824, -7.275392, -7.011388 } },
    { "6", false, 3, 9.5, { -8.995745, -8.58612, -8.213602, -7.873472, -7.561824, -7.275392, -7.011411, -6.767526, -6.54171, -6.332211, -6.137503, -5.956246, -5.787262, -5.629505, -5.482047, -5.34406, -5.2148, -5.093597, -4.979848, -4.872998 } },
    { "6", false, 3, 20, { -8.980776, -8.2259, -7.531415, -6.88837, -6.289668, -5.729586, -5.203441, -4.707355, -4.238079, -3.792862, -3.369357, -2.965545, -2.579676, -2.210222, -1.855843, -1.515359, -1.187718, -0.87199, -0.567335, -0.503627, -0.500627, -0.497628, -0.494627, -0.491627, -0.488627, -0.485627, -0.482627, -0.479627, -0.476628, -0.473627, -0.470628, -0.467628, -0.464628, -0.461627, -0.458628, -0.455628, -0.452628, -0.661147, -1.376795, -2.156744, -3.013699 } },
    { "6", true, 9, 15, { -8.980776, -8.2259, -7.531415, -6.88837, -6.289668, -5.729586, -5.203441, -4.707355, -4.238079, -3.792862, -3.421072, -3.630179, -3.844445, -4.064131, -4.289517, -4.520908, -4.758632, -5.003046, -5.254538, -5.513528, -5.780479, -6.055895, -6.340332, -6.634399, -6.938772, -7.254198, -7.581514, -7.921647, -8.275645, -8.644686, -8.980776 } },
    { "inf", false, 9, 5, { -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699 } },
    { "inf", false, 9, 10.5, { -8.980776, -8.257586, -8.293868, -8.330303, -8.366891, -8.403634, -8.440533, -8.47759, -8.514805, -8.552181, -8.589718, -8.627417, -8.665281, -8.703312, -8.741509, -8.779875, -8.818411, -8.857119, -8.896, -8.935056, -8.974289, -9.013699 } },
    { "inf", false, 3, 8, { -8.995745, -8.58612, -8.213602, -7.962511, -7.887538, -7.812565, -7.737592, -7.662619, -7.587646, -7.512673, -7.4377, -7.362727, -7.287754, -7.21278, -7.137807, -7.062835, -6.987861 } },
    { "inf", false, 3, 12, { -8.980776, -8.2259, -7.693759, -7.618785, -7.543812, -7.468839, -7.393867, -7.318893, -7.243921, -7.168947, -7.093975, -7.019001, -6.944028, -6.869055, -6.794082, -6.719109, -6.644136, -6.569163, -6.49419, -6.419218, -6.344244, -6.269271, -6.194298, -6.119325, -6.044352 } },
    { "inf", true, 9, 9, { -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -9.013699, -8.980776 } },
};

double limitRate (const std::string& setting)
{
    return setting == "inf" ? fx::ReaLimitParams::infiniteRelease : setting == "6" ? 6.0 : setting == "20" ? 19.95 : 39.95;      // the shown value is 6 - 20 log10 n
}

fx::ReaLimitParams limitV2 (const std::string& rate)
{
    fx::ReaLimitParams p;
    p.thresholdDb = -12.005;
    p.ceilingDb = -0.005;
    p.releaseDbPerSec = limitRate (rate);
    return p;
}

/** Gain reduction in dB at sample i (relative to the boosted input, ceiling included). */
double reductionDb (const LimitRun& r, size_t i)        { return dbOf (r.gain (i)); }
} // namespace

ROVOX_TEST (parity, realimit_v2_slow_stage_follows_the_fast_gain_reduction_after_overshoot_steps)
{
    // DC overshoot of `over` dB (threshold -12) for `ms` on a -40 dBFS floor: gain reduction 0 / 1 / 5 / 10 / ... ms after its end (measured: lim4_t-12_r*)
    for (auto& s : limSteps)
    {
        const double floorLin = std::pow (10.0, -40.0 / 20.0), level = std::pow (10.0, (-12.0 + s.over) / 20.0);
        const size_t len = (size_t) std::lround (s.ms * 48.0), start = 24000;
        std::vector<float> x (start + len + 72000, (float) floorLin);
        std::fill (x.begin() + (long) start, x.begin() + (long) (start + len), (float) level);
        const auto r = runLimit (limitV2 (s.rate), x);

        for (auto& pt : s.gr)
            CHECK_NEAR (reductionDb (r, start + len + (size_t) std::lround (pt.first * 48.0)), pt.second, 0.03);
    }
}

ROVOX_TEST (parity, realimit_v2_burst_trains_and_gap_pairs_use_the_ten_ms_hold_window)
{
    const double floorLin = std::pow (10.0, -40.0 / 20.0), hot = std::pow (10.0, (-12.0 + 9.0) / 20.0);

    // 9 dB bursts for 2 s: the gain reduction 3 ms before the 2nd, 3rd, ... burst (measured: lim4 trains)
    for (auto& t : limTrains)
    {
        const size_t period = (size_t) std::lround (t.period * 48.0), on = (size_t) (t.duty * (double) period), start = 24000;
        std::vector<float> x (start + 96000 + 24000, (float) floorLin);

        for (size_t i = 0; i < 96000; ++i)
            if (i % period < on)
                x[start + i] = (float) hot;

        const auto r = runLimit (limitV2 (t.rate), x);

        for (size_t k = 0; k < t.gr.size(); ++k)
            CHECK_NEAR (reductionDb (r, start + (k + 1) * period - 144), t.gr[k], 0.05);
    }

    // two 10 ms overshoots (the second `over` dB, or a single sample peak) with a gap: a same-or-deeper second one within 10 ms means no release at all
    for (auto& pr : limPairs)
    {
        const size_t gap = (size_t) std::lround (pr.gapMs * 48.0), start = 24000;
        const double second = std::pow (10.0, (-12.0 + pr.over) / 20.0);
        std::vector<float> x (start + 480 + gap + 480 + 80000, (float) floorLin);
        size_t from;

        if (pr.impulse)
        {
            x[start] = (float) hot;
            x[start + gap + 1] = (float) hot;
            from = start + 1;
        }
        else
        {
            std::fill (x.begin() + (long) start, x.begin() + (long) (start + 480), (float) hot);
            std::fill (x.begin() + (long) (start + 480 + gap), x.begin() + (long) (start + 960 + gap), (float) second);
            from = start + 480;
        }

        const auto r = runLimit (limitV2 (pr.rate), x);

        for (size_t j = 0; j < pr.gr.size(); ++j)
            CHECK_NEAR (reductionDb (r, from + 24 * j), pr.gr[j], 0.05);
    }
}

ROVOX_TEST (parity, realimit_v2_release_knob_inf_is_150_db_per_second)
{
    // after a long step the slow stage releases at the shown rate; "inf" is 150 dB/s (measured slope 3.124e-3 dB per sample)
    fx::ReaLimitParams p;
    p.thresholdDb = -10.0;
    p.releaseDbPerSec = fx::ReaLimitParams::infiniteRelease;
    CHECK_NEAR (p.releaseDbPerSec, 150.0, 1e-12);
    const auto r = runLimit (p, limiterStep());
    CHECK_NEAR (reductionDb (r, 48000 + 480), -(7.0 + 0.009 - 150.0 * 0.01), 0.03);
    CHECK_NEAR (reductionDb (r, 48000 + 960) - reductionDb (r, 48000 + 480), 150.0 * 0.01, 0.03);
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
    CHECK_NEAR (translatedValue (translateReaperFx ("limit", parseReaperFxEntry (infinite)), "release_rate"), 150.0, 1e-9);
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

//------------------------------------------------------------------------------ ReaVerbate (spec-reaverbate.md v2)
namespace
{
struct VerbCase
{
    const char* name;
    double fs;
    int channel;                                    // the input channel that carries the impulse
    double n[8];                                    // wet, dry, room, damp, width, delay, lowpass, hipass (knob positions)
    std::vector<std::pair<int, double>> taps;       // first output samples above 1e-7 (index, value) of the impulse channel
    std::vector<std::pair<int, double>> spots;      // samples of the left output
};

// measured: unit impulse at 1.0 on one input channel, REAPER 7.81 renders (float32)
const std::vector<VerbCase> verbCases {
    { "verb3_48000_rs30_d0_L", 48000, 0, { 0.497130051, 0, 0, 0, 1, 0, 1, 0 }, { { 1214, 0.0149139017 }, { 1293, 0.0149139017 }, { 1380, -0.0149139017 }, { 1389, 0.0149139017 }, { 1409, -0.0149139017 }, { 1458, -0.0149139017 } }, { { 1116, 0 }, { 2232, 0 }, { 5000, -0.00253862655 }, { 10000, 5.05666103e-05 }, { 20000, 4.63430752e-08 }, { 30000, -3.70445924e-10 } } },
    { "verb3_44100_rs98_d0_R", 44100, 1, { 0.497130051, 0, 0.992647082, 0, 1, 0, 1, 0 }, { { 1139, 0.0149139017 }, { 1211, 0.0149139017 }, { 1300, 0.0149139017 }, { 1315, -0.0149139017 }, { 1342, -0.0149139017 }, { 1379, 0.0149139017 } }, {  } },
    { "verb4_44100_damp50", 44100, 0, { 0.497130051, 0, 0.492647067, 0.5, 1, 0, 1, 0 }, { { 1116, 0.0149139017 }, { 1188, 0.0149139017 }, { 1269, -0.0149139017 }, { 1277, 0.0149139017 }, { 1296, -0.0149139017 }, { 1341, -0.0298278034 } }, { { 1116, 0.0149139017 }, { 2232, 0.0001193114 }, { 5000, 0.00327548338 }, { 10000, -0.00111038901 }, { 20000, 4.07048501e-05 }, { 30000, -1.35400899e-06 } } },
    { "verb4_44100_w05", 44100, 0, { 0.497130051, 0, 0.492647067, 0, 0.75, 0, 1, 0 }, { { 1116, 0.0111854263 }, { 1188, 0.0111854263 }, { 1269, -0.0111854263 }, { 1277, 0.0111854263 }, { 1296, -0.0111854263 }, { 1341, -0.0223708525 } }, { { 1116, 0.0111854263 }, { 2232, 0.0015100328 }, { 5000, 0.00299055036 }, { 10000, -0.00204695924 }, { 20000, 4.50828265e-05 }, { 30000, -7.29432713e-06 } } },
    { "verb4_44100_w0", 44100, 0, { 0.497130051, 0, 0.492647067, 0, 0.5, 0, 1, 0 }, { { 1116, 0.00745695084 }, { 1188, 0.00745695084 }, { 1269, -0.00745695084 }, { 1277, 0.00745695084 }, { 1296, -0.00745695084 }, { 1341, -0.0149139017 } }, { { 1116, 0.00745695084 }, { 2232, 0.00100668846 }, { 5000, 0.00199370016 }, { 10000, -0.00136463961 }, { 20000, 3.00552183e-05 }, { 30000, -4.86288445e-06 } } },
    { "verb4_44100_lp2k", 44100, 0, { 0.497130051, 0, 0.492647067, 0, 1, 0, 0.0999750011, 0 }, { { 1116, 0.000263520342 }, { 1117, 0.000970520661 }, { 1118, 0.00169823086 }, { 1119, 0.00212659431 }, { 1120, 0.00229909946 }, { 1121, 0.00226660119 } }, { { 1116, 0.000263520342 }, { 2232, 0.00245303242 }, { 5000, 0.001564682 }, { 10000, -0.000841991627 }, { 20000, 2.47584703e-05 }, { 30000, -2.42733336e-06 } } },
    { "verb4_44100_hp500", 44100, 0, { 0.497130051, 0, 0.492647067, 0, 1, 0, 1, 0.0249750009 }, { { 1116, 0.0143836401 }, { 1117, -0.00105797581 }, { 1118, -0.0010504754 }, { 1119, -0.00103835447 }, { 1120, -0.0010219895 }, { 1121, -0.0010017521 } }, { { 1116, 0.0143836401 }, { 2232, 0.00110735849 }, { 5000, 0.00172603654 }, { 10000, -0.00256114826 }, { 20000, 1.93481083e-05 }, { 30000, -6.4739711e-06 } } },
    { "verb4_44100_pre50", 44100, 0, { 0.497130051, 0, 0.492647067, 0, 1, 0.0990000032, 1, 0 }, { { 3298, 0.0149139017 }, { 3370, 0.0149139017 }, { 3451, -0.0149139017 }, { 3459, 0.0149139017 }, { 3478, -0.0149139017 }, { 3523, -0.0298278034 } }, { { 1116, 0 }, { 2232, 0 }, { 5000, -0.00186423771 }, { 10000, -0.00138831651 }, { 20000, -0.000107627966 }, { 30000, -1.22405136e-05 } } },
    { "verb4_44100_pre7", 44100, 0, { 0.497130051, 0, 0.492647067, 0, 1, 0.0137, 1, 0 }, { { 1418, 0.0149139017 }, { 1490, 0.0149139017 }, { 1571, -0.0149139017 }, { 1579, 0.0149139017 }, { 1598, -0.0149139017 }, { 1643, -0.0298278034 } }, { { 1116, 0 }, { 2232, 0 }, { 5000, -0.0155505892 }, { 10000, -9.98171381e-05 }, { 20000, 3.92918628e-05 }, { 30000, 3.77023866e-06 } } },
    { "verb4_44100_dry6", 44100, 0, { 0.25, 0.25, 0.492647067, 0, 1, 0, 1, 0 }, { { 0, 0.5 }, { 1116, 0.00749999983 }, { 1188, 0.00749999983 }, { 1269, -0.00749999983 }, { 1277, 0.00749999983 }, { 1296, -0.00749999983 } }, { { 1116, 0.00749999983 }, { 2232, 0.00101250014 }, { 5000, 0.00200520991 }, { 10000, -0.00137251767 }, { 20000, 3.02287262e-05 }, { 30000, -4.89095828e-06 } } },
    { "verb4_96000_d0", 96000, 0, { 0.497130051, 0, 0.492647067, 0, 1, 0, 1, 0 }, { { 2429, 0.0149139017 }, { 2586, 0.0149139017 }, { 2762, -0.0149139017 }, { 2779, 0.0149139017 }, { 2820, -0.0149139017 }, { 2918, -0.0149139017 } }, { { 1116, 0 }, { 2232, 0 }, { 5000, -0.00186423771 }, { 10000, -0.00245792419 }, { 20000, 0.000745251484 }, { 30000, -0.000488610764 } } },
    { "verb5_44100_lp19990", 44100, 0, { 0.497130051, 0, 0.492647067, 0, 1, 0, 0.9995, 0 }, { { 1116, 0.0127506154 }, { 1117, 0.00417534588 }, { 1118, -0.00376077462 }, { 1119, 0.00316998758 }, { 1120, -0.00249166437 }, { 1121, 0.00179861207 } }, { { 1116, 0.0127506154 }, { 2232, -0.000641830731 }, { 5000, 0.00683686417 }, { 10000, -0.00260369247 }, { 20000, 0.000123538091 }, { 30000, -1.06128809e-05 } } },
    { "verb5_44100_hp10", 44100, 0, { 0.497130051, 0, 0.492647067, 0, 1, 0, 1, 0.0005 }, { { 1116, 0.0149032781 }, { 1117, -2.12486812e-05 }, { 1118, -2.12486175e-05 }, { 1119, -2.12485102e-05 }, { 1120, -2.12483592e-05 }, { 1121, -2.12481664e-05 } }, { { 1116, 0.0149032781 }, { 2232, 0.00161541544 }, { 5000, 0.00414096424 }, { 10000, -0.00276446715 }, { 20000, 5.55486658e-05 }, { 30000, -9.84163671e-06 } } },
    { "verb5_48000_lp2k", 48000, 0, { 0.497130051, 0, 0.492647067, 0, 1, 0, 0.1, 0 }, { { 1214, 0.000224975476 }, { 1215, 0.00083477085 }, { 1216, 0.00147942919 }, { 1217, 0.00188708713 }, { 1218, 0.00208745827 }, { 1219, 0.00211595511 } }, { { 1116, 0 }, { 2232, -0.00503242109 }, { 5000, 0.000234015475 }, { 10000, 9.93424401e-05 }, { 20000, 9.34740165e-05 }, { 30000, -2.31672334e-06 } } },
};

Planar verbImpulse (const VerbCase& c, size_t length, int block)
{
    fx::ReaVerb v;
    v.prepare (c.fs);
    v.setParams ({ c.n[0], c.n[1], c.n[2], c.n[3], c.n[4], c.n[5], c.n[6], c.n[7] });
    Planar io (2, std::vector<float> (length, 0.0f));
    io[(size_t) c.channel][0] = 1.0f;

    for (size_t at = 0; at < length; at += (size_t) block)
    {
        float* p[2] = { io[0].data() + at, io[1].data() + at };
        v.process (p, 2, (int) std::min ((size_t) block, length - at));
    }

    return io;
}
} // namespace

ROVOX_TEST (parity, reaverbate_exact_model_matches_the_measured_impulse_responses)
{
    for (auto& c : verbCases)
    {
        const auto y = verbImpulse (c, 31000, 480);
        const auto& out = y[(size_t) c.channel];

        for (auto& [index, value] : c.taps)
            if (std::abs (out[(size_t) index] - value) > 5.0e-8)
            {
                std::cerr << "    " << c.name << " tap " << index << ": " << out[(size_t) index] << " vs " << value << "\n";
                CHECK (false);
            }

        // nothing audible before the first tap
        const auto first = (size_t) c.taps.front().first;

        for (size_t i = 1; i < first; ++i)
            CHECK (std::abs (out[i]) < 1.0e-7f);

        for (auto& [index, value] : c.spots)
            if (std::abs (y[0][(size_t) index] - value) > 5.0e-8)
            {
                std::cerr << "    " << c.name << " sample " << index << ": " << y[0][(size_t) index] << " vs " << value << "\n";
                CHECK (false);
            }

        // width 1: every channel has its own reverberator, the other output stays silent
        if (c.n[4] == 1.0)
            for (float v : y[(size_t) (1 - c.channel)])
                CHECK (v == 0.0f);
    }
}

ROVOX_TEST (parity, reaverbate_lengths_round_down_per_sum_and_the_result_is_block_size_independent)
{
    CHECK (fx::ReaVerb::lineLength (1116, 0, 48000.0) == 1214);
    CHECK (fx::ReaVerb::lineLength (1116, 23, 48000.0) == 1239);           // floor of the sum, not of the parts
    CHECK (fx::ReaVerb::lineLength (1116, 0, 96000.0) == 2429);
    CHECK (fx::ReaVerb::lineLength (153, 23, 44100.0) == 176);

    const auto& c = verbCases[5];                                         // low-pass case: filters and feedback together
    const auto a = verbImpulse (c, 20000, 480), b = verbImpulse (c, 20000, 37);
    CHECK (a == b);
}

ROVOX_TEST (parity, reaverbate_translation_takes_the_knob_positions_and_selects_the_reaper_mode)
{
    const json verb = { { "name", "ReaVerbate" },
                        { "params", json::array ({ prm (0, "Wet", 0.497130051, "-0.1"), prm (1, "Dry", 0.25, "-6.0"), prm (2, "Room size", 0.492647067, "63.5"),
                                                   prm (3, "Dampening", 0.5, "50"), prm (4, "Width", 0.75, "0.50"), prm (5, "Delay", 0.0990000032, "49.5"),
                                                   prm (6, "Lowpass", 0.0999750011, "2000"), prm (7, "Hipass", 0.0249750009, "500"),
                                                   prm (8, "Bypass", 0.0, "normal"), prm (9, "Wet", 0.3, "30"), prm (10, "Delta", 0.0, "normal") }) } };
    const auto t = translateReaperFx ("verb", parseReaperFxEntry (verb));
    CHECK_NEAR (translatedValue (t, "rea_wet"), 0.497130051, 1e-12);
    CHECK_NEAR (translatedValue (t, "rea_dry"), 0.25, 1e-12);
    CHECK_NEAR (translatedValue (t, "rea_room"), 0.492647067, 1e-12);
    CHECK_NEAR (translatedValue (t, "rea_damp"), 0.5, 1e-12);
    CHECK_NEAR (translatedValue (t, "rea_width"), 0.75, 1e-12);
    CHECK_NEAR (translatedValue (t, "rea_delay"), 0.0990000032, 1e-12);
    CHECK_NEAR (translatedValue (t, "rea_lp"), 0.0999750011, 1e-12);
    CHECK_NEAR (translatedValue (t, "rea_hp"), 0.0249750009, 1e-12);
    CHECK_NEAR (translatedValue (t, "reaper_compat"), 1.0, 1e-12);
    CHECK_NEAR (translatedValue (t, "fx_wet"), 30.0, 1e-9);
    CHECK (t.untranslated.empty());
}

//------------------------------------------------------------------------------ ReaFir (spec-reafir.md v2)
namespace
{
const char* const reaFirHeader = "cmZlcu5e7f4CAAAAAQAAAAAAAAACAAAAAAAAAAIAAAABAAAAAAAAAAIAAAAAAAAAWAAAAAEAAAAAABAA";
const char* const reaFirName = "AFByb2dyYW0gMQAQAAAA";
// the saved state of a fresh ReaFir (also of all 4 instances in the user's project) and the same with single fields changed
const char* const reaFirDefault = "nBIAAAAAtMIAAMBBABAAAAAAgD8AAAAAAgAAAAIAAADXOb9DAAAAAKUrSEUAAAAAAAAAAAAAAACtVQM8AACAP7OiBDgAAAA+AAAAAAAAAAACAAACAACAPw==";
const char* const reaFirMode1 = "nBIAAAAAtMIAAMBBABAAAAAAgD8AAAAAAQAAAAIAAADXOb9DAAAAAKUrSEUAAAAAAAAAAAAAAACtVQM8AACAP7OiBDgAAAA+AAAAAAAAAAACAAACAACAPw==";
const char* const reaFirFlatM12 = "nBIAAAAAtMIAAMBBABAAAAAAgD8AAAAAAgAAAAIAAADXOb9DAABAwaUrSEUAAEDBAAAAAAAAAACtVQM8AACAP7OiBDgAAAA+AAAAAAAAAAACAAACAACAPw==";
const char* const reaFirCurve = "nBIAAAAAtMIAAMBBABAAAAAAgD8AAAAAAgAAAAIAAADXOb9DAABAwaUrSEUAAAAAAAAAAAAAAACtVQM8AACAP7OiBDgAAAA+AAAAAAAAAAACAAACAACAPw==";
const char* const reaFirGainHalf = "nBIAAAAAtMIAAMBBABAAAAAAgD8AAAAAAgAAAAIAAADXOb9DAAAAAKUrSEUAAAAAAAAAAAAAAACtVQM8AAAAP7OiBDgAAAA+AAAAAAAAAAACAAACAACAPw==";

std::string reaFirRpp (const char* state)
{
    return std::string ("<REAPER_PROJECT 0.1 \"7.0/macOS-arm64\" 1700000000\n  PANLAW 1\n  PANMODE 3\n  TEMPO 120 4 4 0\n"
                        "  <TRACK {AAAAAAAA-0000-0000-0000-0000000000A1}\n    NAME \"T\"\n    VOLPAN 1 0 -1 -1 1\n    ISBUS 0 0\n"
                        "    <FXCHAIN\n      SHOW 0\n      LASTSEL 0\n      DOCKED 0\n      BYPASS 0 0 0\n"
                        "      <VST \"VST: ReaFir (FFT EQ+Dynamics Processor) (Cockos)\" reafir.vst.dylib 0 \"\" 1919247986<56535472656672726561666972666674> \"\"\n"
                        "        ") + reaFirHeader + "\n        " + state + "\n        " + reaFirName + "\n      >\n      FXID {80AA1093-9FEB-FE49-949F-57955B9893D7}\n    >\n"
           "    <ITEM\n      POSITION 0\n      LENGTH 1\n      <SOURCE WAVE\n        FILE \"item.wav\"\n      >\n    >\n  >\n>\n";
}

struct FirImport
{
    json report;
    std::vector<float> left;
};

FirImport importReaFir (test::Env& env, const char* state)
{
    const auto dir = test::makeTempDir ("parityfir");
    test::writeWav (dir / "item.wav", { std::vector<float> (48000, 0.5f) }, 48000, 24);
    std::ofstream (dir / "t.RPP") << reaFirRpp (state);
    const auto id = env.newProject ("fir");
    auto imp = env.run (id, "import.rpp", { { "path", (dir / "t.RPP").string() } });
    REQUIRE (imp.ok);
    RenderRequest rq;
    rq.tailS = 0.0;
    auto rr = renderProject (env.project (id), rq);
    REQUIRE (rr.ok());
    std::vector<std::vector<float>> ch;
    double rate = 0.0;
    REQUIRE (test::readWav (env.project (id).dir() / rr.value()["file"]["path"].get<std::string>(), ch, rate));
    REQUIRE (! ch.empty());
    return { imp.data, ch[0] };
}

double warningCount (const json& report, const std::string& code)
{
    double n = 0;

    for (auto& w : report["warnings"])
        n += w["code"] == code ? 1 : 0;

    return n;
}

/** Magnitude of an impulse response at a frequency (dB): the DFT of the whole response. */
double responseDb (const std::vector<float>& h, double hz, double fs)
{
    double re = 0, im = 0;

    for (size_t i = 0; i < h.size(); ++i)
    {
        re += h[i] * std::cos (2 * pi * hz * (double) i / fs);
        im -= h[i] * std::sin (2 * pi * hz * (double) i / fs);
    }

    return 20.0 * std::log10 (std::max (1.0e-12, std::hypot (re, im)));
}
} // namespace

ROVOX_TEST (parity, reafir_default_state_is_an_exact_pass_through_without_latency)
{
    fx::ReaFir f;
    f.prepare (48000.0);
    f.setParams ({});
    CHECK (f.isFlat());
    CHECK (f.latencySamples() == 0);
    std::vector<float> x (3000), y;

    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) std::sin (0.01 * (double) i * (double) i);

    y = x;
    float* io[1] = { y.data() };
    f.process (io, 1, (int) y.size());
    CHECK (x == y);

    // a flat curve at -12 dB and an output gain are exact gains, still without latency
    fx::ReaFirParams p;
    p.points = { { 382.45, -12.0 }, { 3202.73, -12.0 } };
    p.outputGain = 0.5;
    f.setParams (p);
    CHECK (f.latencySamples() == 0);
    y = x;
    io[0] = y.data();
    f.process (io, 1, (int) y.size());

    for (size_t i = 0; i < x.size(); ++i)
        CHECK_NEAR (y[i], x[i] * 0.5 * std::pow (10.0, -12.0 / 20.0), 1e-7);
}

ROVOX_TEST (parity, reafir_curve_is_flat_beyond_the_end_points_and_smooth_between_them)
{
    const std::vector<fx::ReaFirPoint> pts { { 382.45, -12.0 }, { 3202.73, 0.0 } };
    CHECK_NEAR (fx::ReaFir::curveGainDb (pts, 100.0), -12.0, 1e-12);
    CHECK_NEAR (fx::ReaFir::curveGainDb (pts, 10000.0), 0.0, 1e-12);
    CHECK_NEAR (fx::ReaFir::curveGainDb (pts, 382.45), -12.0, 1e-12);

    // measured gains (dB) of the response between the points; the smoothstep in log-frequency is within 0.35 dB
    const double measured[][2] = { { 450, -11.757 }, { 600, -10.396 }, { 800, -8.483 }, { 1000, -6.836 }, { 1500, -3.787 }, { 2000, -1.834 }, { 2500, -0.641 }, { 3000, -0.060 } };

    for (auto& m : measured)
        CHECK_NEAR (fx::ReaFir::curveGainDb (pts, m[0]), m[1], 0.35);
}

ROVOX_TEST (parity, reafir_fir_for_a_curve_is_zero_phase_with_the_curve_gain_and_a_quarter_fft_latency)
{
    const double fs = 48000.0;
    fx::ReaFir f;
    f.prepare (fs);
    fx::ReaFirParams p;
    p.points = { { 382.45, -12.0 }, { 3202.73, 0.0 } };
    f.setParams (p);
    CHECK (! f.isFlat());
    CHECK (f.latencySamples() == 1024);
    std::vector<float> h (4096, 0.0f);
    h[0] = 1.0f;
    float* io[1] = { h.data() };

    for (size_t at = 0; at < h.size(); at += 500)
    {
        io[0] = h.data() + at;
        f.process (io, 1, (int) std::min<size_t> (500, h.size() - at));
    }

    for (int k = 1; k <= 1000; ++k)                                             // symmetric around the latency
        CHECK_NEAR (h[(size_t) (1024 - k)], h[(size_t) (1024 + k)], 1e-6);

    CHECK_NEAR (responseDb (h, 100.0, fs), -12.0, 0.4);
    CHECK_NEAR (responseDb (h, 1000.0, fs), -6.84, 0.6);
    CHECK_NEAR (responseDb (h, 8000.0, fs), 0.0, 0.2);
}

ROVOX_TEST (parity, reafir_import_maps_the_default_state_as_exact_pass_through)
{
    test::Env env;
    const auto r = importReaFir (env, reaFirDefault);
    CHECK (r.report["unsupported"].empty());
    REQUIRE (r.report["mapped"].size() == 1);
    CHECK (r.report["mapped"][0]["mapped_to"] == "Rovox FIR EQ");
    CHECK (r.report["mapped"][0]["fft_size"] == 4096);
    CHECK (r.report["mapped"][0]["approximate"] == false);
    CHECK (r.report["warnings"].empty());
    CHECK_NEAR (r.left[30000], 0.5, 1e-6);
    CHECK_NEAR (r.left[100], 0.5, 3e-5);            // 100 samples after the start of the sinc-resampled item the converter still rings (8e-6)
}

ROVOX_TEST (parity, reafir_import_applies_flat_gain_curves_exactly_and_other_curves_approximately)
{
    test::Env env;
    const auto flat = importReaFir (env, reaFirFlatM12);
    CHECK (flat.report["mapped"][0]["approximate"] == false);
    CHECK_NEAR (flat.left[30000], 0.5 * std::pow (10.0, -12.0 / 20.0), 1e-6);
    CHECK_NEAR (flat.left[100], 0.5 * std::pow (10.0, -12.0 / 20.0), 3e-5);

    const auto gain = importReaFir (env, reaFirGainHalf);
    CHECK_NEAR (gain.left[30000], 0.25, 1e-6);

    const auto curve = importReaFir (env, reaFirCurve);
    CHECK (curve.report["mapped"][0]["approximate"] == true);
    CHECK (curve.report["warnings"].empty());
    CHECK_NEAR (curve.left[30000], 0.5 * std::pow (10.0, -12.0 / 20.0), 2e-3);      // DC is below the first point
}

ROVOX_TEST (parity, reafir_import_warns_about_modes_other_than_eq_and_passes_the_audio_through)
{
    test::Env env;
    const auto r = importReaFir (env, reaFirMode1);
    CHECK (r.report["unsupported"].empty());
    CHECK (warningCount (r.report, "REAFIR_MODE_UNSUPPORTED") == 1);
    CHECK_NEAR (r.left[30000], 0.5, 1e-6);
}
