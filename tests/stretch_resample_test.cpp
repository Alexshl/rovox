// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Time-stretch long-run accuracy (no drift) and resampler passband flatness.

#include "FadeStage.h"
#include "Render.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <cmath>
#include <fstream>
#include <numbers>

using namespace rovox;

namespace
{
std::string itemRpp (double length, const std::string& playrate, const std::string& file = "item.wav")
{
    return "<REAPER_PROJECT 0.1 \"7.0/macOS-arm64\" 1700000000\n  TEMPO 120 4 4 0\n"
           "  <TRACK {AAAAAAAA-0000-0000-0000-0000000000B1}\n    NAME \"T\"\n    ISBUS 0 0\n"
           "    <ITEM\n      POSITION 0\n      LENGTH " + std::to_string (length) + "\n      " + playrate + "\n      <SOURCE WAVE\n        FILE \"" + file + "\"\n      >\n    >\n  >\n>\n";
}

std::vector<float> renderRpp (test::Env& env, const fs::path& dir, const std::string& rpp, double& sr)
{
    std::ofstream (dir / "t.RPP") << rpp;
    const auto id = env.newProject ("stretch");
    REQUIRE (env.run (id, "import.rpp", { { "path", (dir / "t.RPP").string() } }).ok);
    RenderRequest rq;
    rq.tailS = 0.0;
    auto rr = renderProject (env.project (id), rq);
    REQUIRE (rr.ok());
    std::vector<std::vector<float>> ch;
    REQUIRE (test::readWav (env.project (id).dir() / rr.value()["file"]["path"].get<std::string>(), ch, sr));
    REQUIRE (! ch.empty());
    return ch[0];
}

/** Imports the RPP, optionally switches the stretch tone off with item.update and/or removes the item-stage from the track, renders the left channel. */
std::vector<float> renderTone (test::Env& env, const fs::path& dir, const std::string& rpp, double& sr, bool toneOff, bool removeStage, bool* flagAfterImport = nullptr)
{
    std::ofstream (dir / "t.RPP") << rpp;
    const auto id = env.newProject ("tone");
    REQUIRE (env.run (id, "import.rpp", { { "path", (dir / "t.RPP").string() } }).ok);
    const auto item = env.ok (id, "item.list")["items"][0];

    if (flagAfterImport != nullptr)
        *flagAfterImport = item["reaper_stretch_tone"].get<bool>();

    if (toneOff)
        env.ok (id, "item.update", { { "item_id", item["item_id"] }, { "reaper_stretch_tone", false } });

    if (removeStage)
        for (auto* t : te::getAudioTracks (env.project (id).edit()))
            for (auto* pl : t->pluginList.getPlugins())
                if (isFadeStage (*pl))
                {
                    pl->deleteFromParent();
                    break;
                }

    RenderRequest rq;
    rq.tailS = 0.0;
    auto rr = renderProject (env.project (id), rq);
    REQUIRE (rr.ok());
    std::vector<std::vector<float>> ch;
    REQUIRE (test::readWav (env.project (id).dir() / rr.value()["file"]["path"].get<std::string>(), ch, sr));
    REQUIRE (! ch.empty());
    return ch[0];
}

/** Hann-windowed single-bin amplitude of `x` over [from, from+n) at `hz`. */
double toneAmplitude (const std::vector<float>& x, size_t from, size_t n, double hz, double sr)
{
    double re = 0, im = 0, w = 0;

    for (size_t i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * (double) i / (double) n);
        const double ph = 2.0 * std::numbers::pi * hz * (double) (from + i) / sr;
        re += win * x[from + i] * std::cos (ph);
        im -= win * x[from + i] * std::sin (ph);
        w += win;
    }

    return 2.0 * std::hypot (re, im) / w;
}

std::vector<float> tones (const std::vector<double>& freqs, double seconds, double sr)
{
    std::vector<float> x ((size_t) (seconds * sr));

    for (size_t i = 0; i < x.size(); ++i)
    {
        double v = 0;

        for (size_t k = 0; k < freqs.size(); ++k)
            v += std::sin (2.0 * std::numbers::pi * freqs[k] * (double) i / sr + 0.7 * (double) k);

        x[i] = (float) (0.05 * v);
    }

    return x;
}

/** Level of each tone (at hz * shift) relative to the 1 kHz one, in dB. */
void checkFlat (const std::vector<float>& out, double sr, const std::vector<double>& freqs, double shift, double tolDb, double maxHz)
{
    const size_t from = (size_t) (0.5 * sr), n = (size_t) (2.0 * sr);
    const double ref = toneAmplitude (out, from, n, freqs[0] * shift, sr);
    REQUIRE (ref > 1e-3);

    for (size_t k = 1; k < freqs.size(); ++k)
    {
        if (freqs[k] > maxHz)
            continue;

        const double db = 20.0 * std::log10 (toneAmplitude (out, from, n, freqs[k] * shift, sr) / ref);
        std::cerr << "    " << freqs[k] << " Hz -> " << db << " dB\n";
        CHECK_NEAR (db, 0.0, tolDb);
    }
}

const std::vector<double> toneFreqs { 1000, 2000, 4000, 8000, 10000, 12500, 14000, 16000, 18000 };
} // namespace

ROVOX_TEST (stretch, signalsmith_input_per_output_ratio_is_exact_for_any_block_size)
{
    const double speed = 1.058333;

    for (const int block : { 256, 480, 512, 1024 })
    {
        te::TimeStretcher ts;
        ts.initialise (48000.0, block, 1, te::TimeStretcher::signalsmithDefault, {}, true);
        REQUIRE (ts.setSpeedAndPitch ((float) (1.0 / speed), 0.0f));
        ts.reset();

        std::vector<float> in (8192, 0.0f), out ((size_t) block, 0.0f);
        const int blocks = 20000;
        double consumed = 0;

        for (int b = 0; b < blocks; ++b)
        {
            const int need = ts.getFramesNeeded();
            REQUIRE (need > 0 && need <= (int) in.size());
            const float* inPtr = in.data();
            float* outPtr = out.data();
            ts.processData (&inPtr, need, &outPtr);
            consumed += need;
        }

        const double expected = (double) blocks * block * speed;
        std::cerr << "    block " << block << ": consumed " << consumed << ", exact " << expected << "\n";
        CHECK_NEAR (consumed, expected, 2.0);
    }
}

ROVOX_TEST (stretch, preserve_pitch_clip_does_not_drift_over_a_minute)
{
    test::Env env;
    const auto dir = test::makeTempDir ("stretchdrift");
    const double rate = 1.058333, fsIn = 48000.0;
    std::vector<float> src ((size_t) (70.0 * fsIn), 0.0f);

    for (int k = 0; k < 66; ++k)
        for (int i = 0; i < 24; ++i)
            src[(size_t) (k * fsIn) + (size_t) i] = 0.8f * std::sin ((float) i * 0.13f);       // short burst each second of source time

    test::writeWav (dir / "item.wav", { src }, fsIn, 24);
    double sr = 0;
    const auto out = renderRpp (env, dir, itemRpp (60.0, "PLAYRATE " + std::to_string (rate) + " 1 0 -1 0 0.0025"), sr);

    // centroid of the energy around the expected position of click k
    auto lag = [&] (int k)
    {
        const double expected = k / rate * sr;
        const auto lo = (size_t) std::max (0.0, expected - 0.02 * sr), hi = std::min ((size_t) (expected + 0.02 * sr), out.size());
        double num = 0, den = 0;

        for (size_t i = lo; i < hi; ++i)
        {
            num += (double) i * out[i] * out[i];
            den += (double) out[i] * out[i];
        }

        return den > 0 ? num / den - expected : 1e9;
    };

    const double early = lag (3), late = lag (55);
    std::cerr << "    lag at click 3: " << early << " samples, at click 55: " << late << " samples, drift " << late - early << "\n";
    CHECK (std::abs (early) < 0.02 * sr);
    CHECK_NEAR (late - early, 0.0, 6.0);        // 52 s * 2.46e-4 = 560 samples before the fix
}

ROVOX_TEST (stretch, sample_rate_conversion_48_to_44_1_is_flat_to_18_khz)
{
    test::Env env;
    const auto dir = test::makeTempDir ("src4844");
    test::writeWav (dir / "item.wav", { tones (toneFreqs, 4.0, 48000.0) }, 48000.0, 24);
    double sr = 0;
    const auto out = renderRpp (env, dir, itemRpp (3.0, "PLAYRATE 1 0 0 -1 0 0.0025"), sr);
    CHECK_NEAR (sr, 44100.0, 0.5);
    checkFlat (out, sr, toneFreqs, 1.0, 0.1, 18000.0);
}

ROVOX_TEST (stretch, varispeed_resampling_is_flat_to_18_khz)
{
    test::Env env;
    const auto dir = test::makeTempDir ("varispeed");
    test::writeWav (dir / "item.wav", { tones (toneFreqs, 4.0, 48000.0) }, 48000.0, 24);
    double sr = 0;
    const double rate = 1.0583;
    const auto out = renderRpp (env, dir, itemRpp (3.0, "PLAYRATE " + std::to_string (rate) + " 0 0 -1 0 0.0025"), sr);
    checkFlat (out, sr, toneFreqs, rate, 0.1, 18000.0);
}

ROVOX_TEST (stretch, reaper_stretch_tone_off_is_bit_exact_and_on_has_the_fitted_response)
{
    test::Env env;
    const auto dir = test::makeTempDir ("stretchtone");
    const std::vector<double> freqs { 1000, 100, 2000, 8000, 12500, 16000, 18000 };
    test::writeWav (dir / "item.wav", { tones (freqs, 4.0, 48000.0) }, 48000.0, 24);
    const auto rpp = itemRpp (3.0, "PLAYRATE 1.0583 1 0 -1 0 0.0025");
    double sr = 0;
    bool flagged = false;
    const auto on = renderTone (env, dir, rpp, sr, false, false, &flagged);
    CHECK (flagged);                                                    // set by the RPP import: stretched with preserve pitch
    const auto off = renderTone (env, dir, rpp, sr, true, false);
    const auto none = renderTone (env, dir, rpp, sr, true, true);       // no item stage on the track at all
    REQUIRE (off.size() == none.size());
    CHECK (off == none);                                                // flag off: bit-exact

    // expected: low shelf +0.80 dB at 150 Hz and high shelf -2.05 dB at 15.5 kHz (RBJ, S = 1) at 44.1 kHz
    const std::vector<std::pair<double, double>> expected { { 100, 0.668 }, { 1000, 0.0 }, { 8000, -0.022 }, { 12500, -0.269 }, { 16000, -1.209 }, { 18000, -1.828 } };
    const size_t from = (size_t) (0.5 * sr), n = (size_t) (2.0 * sr);
    REQUIRE (on.size() == off.size());

    for (auto& [hz, db] : expected)
    {
        const double got = 20.0 * std::log10 (toneAmplitude (on, from, n, hz, sr) / toneAmplitude (off, from, n, hz, sr));
        std::cerr << "    " << hz << " Hz: " << got << " dB (expected " << db << ")\n";
        CHECK_NEAR (got, db, 0.05);
    }
}

ROVOX_TEST (stretch, reaper_stretch_tone_is_only_set_for_stretched_preserve_pitch_items)
{
    test::Env env;
    const auto dir = test::makeTempDir ("stretchflag");
    test::writeWav (dir / "item.wav", { tones ({ 1000 }, 4.0, 48000.0) }, 48000.0, 24);
    double sr = 0;
    bool flagged = true;
    renderTone (env, dir, itemRpp (1.0, "PLAYRATE 1 1 0 -1 0 0.0025"), sr, false, false, &flagged);
    CHECK (! flagged);                                                  // not stretched
    flagged = true;
    renderTone (env, dir, itemRpp (1.0, "PLAYRATE 1.0583 0 0 -1 0 0.0025"), sr, false, false, &flagged);
    CHECK (! flagged);                                                  // resampled (pitch follows speed)
}
