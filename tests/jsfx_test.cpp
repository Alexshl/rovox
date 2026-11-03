// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER JS effects (JSFX) through ysfx: lookup below ROVOX_JSFX_PATH (case-insensitive), the catalog, fx.add / fx.params / fx.update with slider
// units, the audio (gain, filter, latency) in offline renders and block by block, and the RPP import of `<JS ...>` blocks. The effects are
// three tiny files written for the tests (tests/fixtures/jsfx), no REAPER files.
#include "JobManager.h"
#include "JsfxPlugin.h"
#include "PluginHost.h"
#include "Render.h"
#include "RenderJobs.h"
#include "RppImport.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>

using namespace rovox;

namespace
{
using Planar = std::vector<std::vector<float>>;
constexpr double pi = 3.14159265358979323846;

fs::path fixtureDir()
{
    const char* dir = std::getenv ("ROVOX_TEST_JSFX_DIR");
    return dir != nullptr ? fs::path (dir) : fs::path ("tests/fixtures/jsfx");
}

/** ROVOX_JSFX_PATH points at the fixtures for the life of the object. */
struct JsfxPath
{
    JsfxPath()
    {
        const char* old = std::getenv ("ROVOX_JSFX_PATH");
        hadOld = old != nullptr;

        if (hadOld)
            oldValue = old;

        ::setenv ("ROVOX_JSFX_PATH", fixtureDir().string().c_str(), 1);
        jsfxRescan();
    }

    ~JsfxPath()
    {
        if (hadOld)
            ::setenv ("ROVOX_JSFX_PATH", oldValue.c_str(), 1);
        else
            ::unsetenv ("ROVOX_JSFX_PATH");

        jsfxRescan();
    }

    bool hadOld = false;
    std::string oldValue;
};

std::string sineProject (test::Env& env, double hz, double seconds = 3.0)
{
    const auto id = env.newProject ("jsfx");
    const auto dir = test::makeTempDir ("jsfx-sine");
    test::writeWav (dir / "sine.wav", { test::sine (hz, -18.0, seconds, 44100.0) }, 44100.0, 24);
    auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "sine.wav").string() } } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    return id;
}

std::string firstTrack (test::Env& env, const std::string& id)
{
    return env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();
}

/** Peak of channel 0 between 1.0 s and 2.5 s of a render without a tail. */
double renderPeak (test::Env& env, const std::string& id, const char* name)
{
    RenderRequest req;
    req.name = name;
    req.tailS = 0.0;
    auto r = renderProject (env.project (id), req);
    REQUIRE (r.ok());
    Planar ch;
    double sr = 0;
    REQUIRE (test::readWav (env.project (id).rendersDir() / (std::string (name) + ".wav"), ch, sr));
    double peak = 0.0;

    for (size_t i = (size_t) sr; i < std::min (ch[0].size(), (size_t) (2.5 * sr)); ++i)
    {
        REQUIRE (std::isfinite (ch[0][i]));
        peak = std::max (peak, (double) std::abs (ch[0][i]));
    }

    return peak;
}

json findParam (const json& fxInfo, const std::string& ident)
{
    for (auto& p : fxInfo["params"])
        if (p["ident"] == ident)
            return p;

    return nullptr;
}

double paramValue (test::Env& env, const std::string& project, const std::string& pluginId, const std::string& ident)
{
    return findParam (env.ok (project, "fx.params", { { "plugin_id", pluginId } })["fx"], ident)["value"].get<double>();
}
} // namespace

//==============================================================================
ROVOX_TEST (jsfx, lookup_is_case_insensitive_and_stays_inside_the_roots)
{
    JsfxPath path;

    auto a = findJsfx ("ROVOX_TEST/gain");
    REQUIRE (a.has_value());
    CHECK (a->rel == "rovox_test/Gain");
    CHECK (findJsfx ("rovox_test/lowpass")->rel == "rovox_test/lowpass.jsfx");
    CHECK (findJsfx ("Rovox_Test\\LOWPASS.jsfx")->rel == "rovox_test/lowpass.jsfx");
    CHECK (! findJsfx ("rovox_test/missing").has_value());
    CHECK (! findJsfx ("../jsfx/rovox_test/Gain").has_value());
    CHECK (! findJsfx ((fixtureDir() / "rovox_test" / "Gain").string()).has_value());
    CHECK (! findJsfx ("").has_value());

    ::setenv ("ROVOX_JSFX_PATH", "/nonexistent-a:/nonexistent-b", 1);
    CHECK (jsfxRoots().size() == 2);
    CHECK (! findJsfx ("rovox_test/Gain").has_value());
    ::setenv ("ROVOX_JSFX_PATH", fixtureDir().string().c_str(), 1);
    CHECK (findJsfx ("rovox_test/Gain").has_value());
}

ROVOX_TEST (jsfx, catalog_lists_the_effects_and_fx_add_params_and_update_work)
{
    JsfxPath path;
    test::Env env;
    auto cat = env.ok ("", "plugin.catalog", { { "format", "JSFX" }, { "limit", 50 } });
    REQUIRE (cat["total"].get<int>() == 4);
    bool sawGain = false;

    for (auto& p : cat["plugins"])
    {
        CHECK (p["format"] == "JSFX");
        CHECK (p["kind"] == "effect");
        CHECK (std::find (p["categories"].begin(), p["categories"].end(), "jsfx") != p["categories"].end());
        CHECK (p["plugin_uid"].get<std::string>().rfind ("jsfx:rovox_test/", 0) == 0);

        if (p["plugin_uid"] == "jsfx:rovox_test/Gain")
        {
            sawGain = true;
            CHECK (p["name"] == "Rovox test gain");
        }
    }

    CHECK (sawGain);
    CHECK (env.ok ("", "plugin.catalog", { { "query", "one-pole" } })["total"] == 1);
    CHECK (env.ok ("", "plugin.probe", { { "plugin_uid", "jsfx:rovox_test/Gain" } })["probe"]["builtin"] == true);

    const auto id = sineProject (env, 330.0);
    const auto track = firstTrack (env, id);
    auto added = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "jsfx:rovox_test/gain" } } } });
    CHECK (added["plugin"]["name"] == "Rovox test gain");
    CHECK (added["plugin"]["type"] == "rovoxjsfx");
    CHECK (! env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "jsfx:rovox_test/nope" } } } }).ok);

    auto list = env.ok (id, "fx.list", { { "target", track } })["fx"];
    REQUIRE (list.size() >= 2);
    CHECK (list[0]["name"] == "Rovox test gain");
    CHECK (list[1]["is_fader"] == true);

    const auto fxId = added["plugin_id"].get<std::string>();
    auto info = env.ok (id, "fx.params", { { "plugin_id", fxId } })["fx"];
    CHECK (info["jsfx"] == true);
    CHECK (info["jsfx_file"] == "rovox_test/Gain");
    CHECK (info["units"]["s1"] == "dB");
    CHECK (info["units"]["s2"] == "choice: Normal|Mute|Invert");
    REQUIRE (info["params"].size() == 2);
    CHECK (findParam (info, "s1")["name"] == "Gain (dB)");
    CHECK_NEAR (findParam (info, "s1")["min"].get<double>(), -60.0, 1e-4);
    CHECK_NEAR (findParam (info, "s1")["max"].get<double>(), 12.0, 1e-4);
    CHECK (findParam (info, "s2")["display"] == "Normal");

    const auto steps = env.ok (id, "history.list")["undo"].size();
    env.ok (id, "fx.update", { { "plugin_id", fxId }, { "params", { { "s1", "-6 dB" }, { "s2", "Invert" } } } });
    CHECK (env.ok (id, "history.list")["undo"].size() == steps + 1);
    info = env.ok (id, "fx.params", { { "plugin_id", fxId } })["fx"];
    CHECK_NEAR (findParam (info, "s1")["value"].get<double>(), -6.0, 1e-4);
    CHECK (findParam (info, "s2")["display"] == "Invert");
    CHECK (! env.run (id, "fx.update", { { "plugin_id", fxId }, { "params", { { "s1", 40.0 } } } }).ok);      // out of the slider's range
    env.ok (id, "undo");
    CHECK_NEAR (paramValue (env, id, fxId, "s1"), 0.0, 1e-4);
    env.ok (id, "redo");

    // the slider values live in the plugin state, so that save and render copies see them
    auto* plugin = env.project (id).findPlugin (fxId);
    REQUIRE (plugin != nullptr);
    CHECK (toStd (plugin->state["jsfx_file"].toString()) == "rovox_test/Gain");
    CHECK_NEAR ((double) plugin->state["s1"], -6.0, 1e-4);
}

ROVOX_TEST (jsfx, gain_and_filter_change_the_rendered_audio_as_designed)
{
    JsfxPath path;
    test::Env env;

    // gain: -6.0206 dB halves the amplitude, "Invert" flips it, "Mute" silences
    {
        const auto id = sineProject (env, 330.0);
        const auto track = firstTrack (env, id);
        const double dry = renderPeak (env, id, "dry");
        CHECK (dry > 0.1);
        const auto fxId = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "jsfx:rovox_test/Gain" } } } })["plugin_id"].get<std::string>();
        CHECK_NEAR (renderPeak (env, id, "unity") / dry, 1.0, 0.01);

        env.ok (id, "fx.update", { { "plugin_id", fxId }, { "params", { { "s1", -6.0206 } } } });
        CHECK_NEAR (renderPeak (env, id, "half") / dry, 0.5, 0.01);
        env.ok (id, "fx.update", { { "plugin_id", fxId }, { "params", { { "s2", "Invert" } } } });
        CHECK_NEAR (renderPeak (env, id, "inv") / dry, 0.5, 0.01);
        env.ok (id, "fx.update", { { "plugin_id", fxId }, { "params", { { "s2", 1 } } } });

        RenderRequest req;
        req.name = "mute";
        req.tailS = 0.0;
        req.failOn.clear();
        auto r = renderProject (env.project (id), req);
        REQUIRE (r.ok());
        Planar ch;
        double sr = 0;
        REQUIRE (test::readWav (env.project (id).rendersDir() / "mute.wav", ch, sr));
        double peak = 0.0;

        for (float v : ch[0])
            peak = std::max (peak, (double) std::abs (v));

        CHECK (peak < 1.0e-6);
    }

    // one-pole low-pass at 1 kHz on a 5 kHz sine: |H| = (1 - a) / sqrt(1 - 2 a cos(w) + a^2)
    {
        const double fs = 44100.0, f = 5000.0, cutoff = 1000.0;
        const auto id = sineProject (env, f);
        const auto track = firstTrack (env, id);
        const double dry = renderPeak (env, id, "dry");
        env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "jsfx:rovox_test/lowpass.jsfx" } } } });
        const double wet = renderPeak (env, id, "lp");
        const double a = std::exp (-2.0 * pi * cutoff / fs), w = 2.0 * pi * f / fs;
        const double expected = (1.0 - a) / std::sqrt (1.0 - 2.0 * a * std::cos (w) + a * a);
        CHECK_NEAR (wet / dry, expected, 0.02 * expected);
        CHECK (wet / dry < 0.3);
    }
}

ROVOX_TEST (jsfx, the_render_child_process_loads_the_jsfx_effects_too)
{
    JsfxPath path;
    test::Env env;
    const auto id = sineProject (env, 330.0, 2.0);
    const auto track = firstTrack (env, id);
    auto added = env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "jsfx:rovox_test/Gain" } } } });
    env.ok (id, "fx.update", { { "plugin_id", added["plugin_id"] }, { "params", { { "s1", -6.0206 } } } });
    CHECK (editHasExternalPlugins (env.project (id).edit()));          // so the job renders in `rovox render-child`

    JobManager jobs { fs::path() };
    JobService service (env.pm, jobs, env.executor);
    auto started = service.startRender ({ "local", id, Origin::system() }, { { "name", "child" }, { "check", true } }, false);
    REQUIRE (started.ok());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (90);
    json job;

    for (;;)
    {
        test::pumpMessages (10);
        auto j = jobs.get (started.value().jobId);

        if (j && isFinished (j->state))
        {
            job = j->toJson();
            break;
        }

        REQUIRE (std::chrono::steady_clock::now() < deadline);
    }

    if (job["state"] != "succeeded")
        std::cerr << "    " << job.dump() << "\n";

    REQUIRE (job["state"] == "succeeded");
    Planar ch;
    double sr = 0;
    REQUIRE (test::readWav (env.project (id).rendersDir() / "child.wav", ch, sr));
    double peak = 0.0;

    for (float v : ch[0])
        peak = std::max (peak, (double) std::abs (v));

    CHECK_NEAR (peak, std::pow (10.0, -18.0 / 20.0) * 0.5, 0.03 * 0.063);       // the -6 dB of the effect, not the unprocessed -18 dBFS

    jobs.shutdown();

    // a JSFX whose file cannot be found makes the preflight fail instead of passing the audio through unchanged
    const auto id2 = sineProject (env, 330.0, 1.0);
    auto* track2 = te::getAudioTracks (env.project (id2).edit())[0];
    auto state = te::createValueTree (te::IDs::PLUGIN, te::IDs::type, juce::String ("rovoxjsfx"));
    state.setProperty ("jsfx_file", "nothere/missing", nullptr);
    auto plugin = env.project (id2).edit().getPluginCache().createNewPlugin (state);
    REQUIRE (plugin != nullptr);
    track2->pluginList.insertPlugin (plugin, 0, nullptr);
    CHECK (! jsfxPluginExtras (*plugin)["loaded"].get<bool>());

    RenderRequest req;
    req.name = "missing";
    req.tailS = 0.0;
    auto r = renderProject (env.project (id2), req);
    REQUIRE (! r.ok());
    CHECK (r.error().code == errc::preflightFailed);
    bool notLoaded = false;

    for (auto& f : r.error().details["details"])
        notLoaded = notLoaded || f["check"] == "plugin_not_loaded";

    CHECK (notLoaded);
}

ROVOX_TEST (jsfx, pdc_delay_is_reported_and_the_audio_is_delayed_block_by_block)
{
    JsfxPath path;
    test::Env env;
    const auto id = env.newProject ("jsfx pdc");
    auto created = createJsfxPlugin (env.project (id).edit(), "rovox_test/latency");
    REQUIRE (created.ok());
    auto plugin = created.value();

    plugin->initialise (te::PluginInitialisationInfo { te::TimePosition(), 44100.0, 256 });
    CHECK (jsfxPluginExtras (*plugin)["latency_samples"].get<int>() == 64);

    // an impulse comes out 64 samples later, also across block boundaries (blocks of 100 and 156 samples)
    juce::AudioBuffer<float> buffer (2, 256);
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);
    buffer.setSample (1, 0, 0.5f);

    for (auto [start, count] : { std::pair<int, int> { 0, 100 }, std::pair<int, int> { 100, 156 } })
    {
        te::PluginRenderContext ctx (&buffer, start, count, nullptr, 0.0, te::TimeRange(), true, false, true, false);
        plugin->applyToBuffer (ctx);
    }

    for (int i = 0; i < 256; ++i)
    {
        CHECK_NEAR (buffer.getSample (0, i), i == 64 ? 1.0 : 0.0, 1.0e-6);
        CHECK_NEAR (buffer.getSample (1, i), i == 64 ? 0.5 : 0.0, 1.0e-6);
    }
}

ROVOX_TEST (jsfx, an_item_at_zero_through_a_plugin_with_latency_renders_sample_exact_from_sample_zero)
{
    JsfxPath path;
    test::Env env;
    const auto id = sineProject (env, 330.0, 1.0);
    const auto track = firstTrack (env, id);

    auto render = [&] (const char* name)
    {
        RenderRequest req;
        req.name = name;
        req.tailS = 0.0;
        auto r = renderProject (env.project (id), req);
        REQUIRE (r.ok());
        Planar ch;
        double sr = 0;
        REQUIRE (test::readWav (env.project (id).rendersDir() / (std::string (name) + ".wav"), ch, sr));
        return ch;
    };

    const auto dry = render ("dry");
    env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "jsfx:rovox_test/latency" } } } });
    const auto wet = render ("wet");
    REQUIRE (dry.size() == wet.size());
    REQUIRE (wet[0].size() == dry[0].size());
    double worst = 0.0;

    for (size_t c = 0; c < dry.size(); ++c)
        for (size_t i = 0; i < dry[c].size(); ++i)
            worst = std::max (worst, (double) std::abs (dry[c][i] - wet[c][i]));

    CHECK (worst == 0.0);
}

ROVOX_TEST (jsfx, rpp_js_blocks_become_jsfx_plugins_with_the_slider_values_of_the_project)
{
    JsfxPath path;
    test::Env env;
    const auto dir = test::makeTempDir ("jsfx-rpp");
    std::ofstream (dir / "js.RPP") << R"RPP(<REAPER_PROJECT 0.1 "7.0/macOS-arm64" 1700000000
  TEMPO 120 4 4 0
  <TRACK {AAAAAAAA-0000-0000-0000-0000000000A1}
    NAME "JS"
    ISBUS 0 0
    <FXCHAIN
      BYPASS 0 0 0
      <JS ROVOX_TEST/gain ""
        -6 2 - - -
      >
      FLOATPOS 0 0 0 0
      FXID {11111111-1111-1111-1111-111111111111}
      WAK 0 0
      BYPASS 1 0 0
      <JS "rovox_test/lowpass.jsfx" ""
        5000
      >
      FXID {22222222-2222-2222-2222-222222222222}
      BYPASS 0 0 0
      <JS nothere/missing ""
        1 2
      >
      FXID {33333333-3333-3333-3333-333333333333}
    >
  >
>
)RPP";

    const auto id = env.newProject ("js import");
    auto r = env.run (id, "import.rpp", { { "path", (dir / "js.RPP").string() } });
    REQUIRE (r.ok);
    const auto& rep = r.data;
    REQUIRE (rep["mapped"].size() == 2);
    CHECK (rep["mapped"][0]["name"] == "ROVOX_TEST/gain");
    CHECK (rep["mapped"][0]["mapped_to"] == "JSFX rovox_test/Gain");
    CHECK (rep["mapped"][0]["status"] == "mapped (JSFX, slider values from the project)");
    CHECK (rep["mapped"][0]["sliders_set"] == 2);
    CHECK (rep["mapped"][1]["mapped_to"] == "JSFX rovox_test/lowpass.jsfx");
    CHECK (rep["mapped"][1]["enabled"] == false);
    REQUIRE (rep["unsupported"].size() == 1);
    CHECK (rep["unsupported"][0]["name"] == "nothere/missing");
    CHECK (rep["unsupported"][0]["reason"] == "JSFX file not found under ROVOX_JSFX_PATH");

    const auto gainId = rep["mapped"][0]["plugin_id"].get<std::string>(), lpId = rep["mapped"][1]["plugin_id"].get<std::string>();
    CHECK_NEAR (paramValue (env, id, gainId, "s1"), -6.0, 1e-4);
    CHECK_NEAR (paramValue (env, id, gainId, "s2"), 2.0, 1e-4);
    CHECK_NEAR (paramValue (env, id, lpId, "s1"), 5000.0, 1e-2);
    CHECK (env.ok (id, "fx.params", { { "plugin_id", gainId } })["fx"]["enabled"] == true);
    CHECK (env.ok (id, "fx.params", { { "plugin_id", lpId } })["fx"]["enabled"] == false);

    // without the effect directory the same project imports, the JS effects are unsupported with the reason
    ::setenv ("ROVOX_JSFX_PATH", "/nonexistent-jsfx", 1);
    jsfxRescan();
    const auto id2 = env.newProject ("js import 2");
    auto r2 = env.run (id2, "import.rpp", { { "path", (dir / "js.RPP").string() } });
    REQUIRE (r2.ok);
    CHECK (r2.data["mapped"].empty());
    REQUIRE (r2.data["unsupported"].size() == 3);
    CHECK (r2.data["unsupported"][0]["reason"] == "JSFX file not found under ROVOX_JSFX_PATH");
}

//==============================================================================
// A de-esser JSFX (rovox_test/deess, built like the published split-band ones) on a vocal-like signal: its output must be what its own code defines.
// The reference below is the same algorithm in C++; the JSFX code is the specification, so a host problem (sample rate, slider values reaching
// @slider, @block/samplesblock, spl0/spl1 routing, state across blocks) shows up as a difference.
namespace
{
struct DeessSettings
{
    int mono = 0;
    double freq = 4000.0, thresholdDb = -25.0, ratio = 4.0;
};

std::vector<std::vector<double>> deessReference (const std::vector<std::vector<float>>& in, double fs, const DeessSettings& s)
{
    const double thr = std::pow (10.0, 2.0 * (s.thresholdDb / 80.0 + 1.0) - 2.0), rat = (s.ratio - 1.0) / 19.0;
    const double att = std::pow (10.0, -0.002), rel = std::pow (10.0, -3.5);
    const double fpi = pi * s.freq, wc = 2.0 * fpi, wc2 = wc * wc, wc22 = 2.0 * wc2, k = wc / std::tan (fpi / fs), k2 = k * k, k22 = 2.0 * k2, wck2 = 2.0 * wc * k;
    const double tmpk = k2 + wc2 + wck2;
    const double b1 = (-k22 + wc22) / tmpk, b2 = (-wck2 + k2 + wc2) / tmpk;
    const double lp0 = wc2 / tmpk, lp1 = wc22 / tmpk, lp2 = wc2 / tmpk, hp0 = k2 / tmpk, hp1 = -k22 / tmpk, hp2 = k2 / tmpk;

    struct Biquad { double x0 = 0, x1 = 0; double run (double a0, double a1, double a2, double b1_, double b2_, double in)
    { const double o = a0 * in + x0; x0 = a1 * in - b1_ * o + x1; x1 = a2 * in - b2_ * o; return o; } };

    const size_t n = in[0].size();
    std::vector<std::vector<double>> out (2, std::vector<double> (n));
    Biquad lo[2], hi[2];
    double env = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        const double l = in[0][i], r = in[1][i];
        double band[2], low[2];
        const int chans = s.mono ? 1 : 2;

        for (int c = 0; c < chans; ++c)
        {
            const double x = s.mono ? (l + r) / 2.0 : (c == 0 ? l : r);
            low[c] = lo[c].run (lp0, lp1, lp2, b1, b2, x);
            band[c] = -hi[c].run (hp0, hp1, hp2, b1, b2, x);
        }

        const double det = s.mono ? std::abs (band[0]) : std::max (std::abs (band[0]), std::abs (band[1]));
        env = det > env ? env + att * (det - env) : env * (1.0 - rel);
        const double cgain = env > thr ? 1.0 + rat * (env / thr - 1.0) : 1.0;

        if (env < 1.0e-10)
            env = 0.0;

        for (int c = 0; c < 2; ++c)
        {
            const int src = s.mono ? 0 : c;
            out[(size_t) c][i] = low[src] + band[src] / cgain;
        }
    }

    return out;
}

double rmsOf (const std::vector<double>& v, size_t from, size_t to)
{
    double sum = 0.0;

    for (size_t i = from; i < to; ++i)
        sum += v[i] * v[i];

    return std::sqrt (sum / (double) (to - from));
}
} // namespace

ROVOX_TEST (jsfx, a_de_esser_jsfx_reduces_sibilance_exactly_as_its_code_defines_in_mono_and_stereo)
{
    JsfxPath path;
    test::Env env;
    const double fs = 44100.0;
    const size_t n = (size_t) (3.0 * fs);

    // a voiced tone (harmonics of 200 Hz, nothing above the split) with two loud sibilant bursts (5 to 9 kHz) after a quiet first second
    std::vector<std::vector<float>> signal (2, std::vector<float> (n));
    const double sib[] = { 5100.0, 6300.0, 7000.0, 7900.0, 8800.0 };

    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        double v = 0.0;

        for (int h = 1; h <= 8; ++h)
            v += 0.06 / h * std::sin (2.0 * pi * 200.0 * h * t);

        for (double start : { 1.0, 2.0 })
            if (t >= start && t < start + 0.3)
            {
                const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (t - start) / 0.3);

                for (size_t k = 0; k < 5; ++k)
                    v += 0.16 * w * std::sin (2.0 * pi * sib[k] * t + 0.7 * (double) k);
            }

        signal[0][i] = (float) v;
        signal[1][i] = (float) (0.5 * v);
    }

    const auto dir = test::makeTempDir ("jsfx-deess");
    test::writeWav (dir / "vocal.wav", signal, fs, 32);

    for (const int mono : { 1, 0 })
    {
        // Threshold -50 dB and ratio 20: the band above 4 kHz is pressed hard; the slider line is a REAPER one ("-" = default).
        std::ofstream (dir / "deess.RPP") << "<REAPER_PROJECT 0.1 \"7.0/macOS-arm64\" 1700000000\n  SAMPLERATE 44100 0 0\n  TEMPO 120 4 4 0\n"
            "  <TRACK {AAAAAAAA-0000-0000-0000-0000000000B1}\n    NAME \"vox\"\n    ISBUS 0 0\n    <FXCHAIN\n      BYPASS 0 0 0\n"
            "      <JS rovox_test/deess \"\"\n        " << mono << " - -50 20\n      >\n      FXID {44444444-4444-4444-4444-444444444444}\n    >\n"
            "    <ITEM\n      POSITION 0\n      LENGTH 3\n      NAME \"vocal.wav\"\n      VOLPAN 1 0 1 -1\n      SOFFS 0\n      PLAYRATE 1 1 0 -1 0 0.0025\n"
            "      <SOURCE WAVE\n        FILE \"" << (dir / "vocal.wav").string() << "\"\n      >\n    >\n  >\n>\n";

        const auto id = env.newProject ("deess");
        auto imp = env.run (id, "import.rpp", { { "path", (dir / "deess.RPP").string() } });
        REQUIRE (imp.ok);
        REQUIRE (imp.data["mapped"].size() == 1);
        const auto fxId = imp.data["mapped"][0]["plugin_id"].get<std::string>();

        // the project's slider values arrive as they are in the file, "-" stays the default of the effect
        CHECK_NEAR (paramValue (env, id, fxId, "s1"), (double) mono, 1e-6);
        CHECK_NEAR (paramValue (env, id, fxId, "s2"), 4000.0, 1e-3);
        CHECK_NEAR (paramValue (env, id, fxId, "s3"), -50.0, 1e-4);
        CHECK_NEAR (paramValue (env, id, fxId, "s4"), 20.0, 1e-4);

        RenderRequest req;
        req.name = "deess";
        req.tailS = 0.0;
        auto r = renderProject (env.project (id), req);
        REQUIRE (r.ok());
        Planar wet;
        double rate = 0;
        REQUIRE (test::readWav (env.project (id).rendersDir() / "deess.wav", wet, rate));
        REQUIRE (wet.size() == 2);
        REQUIRE (rate == fs);
        REQUIRE (wet[0].size() >= n);

        DeessSettings s;
        s.mono = mono;
        s.thresholdDb = -50.0;
        s.ratio = 20.0;
        const auto ref = deessReference (signal, fs, s);
        const size_t from = (size_t) (0.8 * fs);       // after the first block, where @block ramps the coefficients up from zero
        double worst = 0.0;

        for (size_t c = 0; c < 2; ++c)
            for (size_t i = from; i < n; ++i)
                worst = std::max (worst, std::abs ((double) wet[c][i] - ref[c][i]));

        CHECK (worst < 1.0e-4);

        // the sibilance is really pressed: the burst is quieter than the uncompressed sum, the voiced part before it is untouched
        std::vector<double> dryBurst, wetBurst, dryVoiced, wetVoiced;
        const size_t b0 = (size_t) (1.0 * fs), b1 = (size_t) (1.3 * fs), v0 = (size_t) (0.5 * fs), v1 = (size_t) (0.9 * fs);
        auto dryD = deessReference (signal, fs, [&] { auto d = s; d.thresholdDb = 0.0; d.ratio = 1.0; return d; }());        // ratio 1: no gain change
        std::vector<double> wet0 (wet[0].begin(), wet[0].end());
        const double burstRatio = rmsOf (wet0, b0, b1) / rmsOf (dryD[0], b0, b1);
        CHECK (burstRatio < 0.6);
        CHECK_NEAR (rmsOf (wet0, v0, v1) / rmsOf (dryD[0], v0, v1), 1.0, 0.01);
    }
}
