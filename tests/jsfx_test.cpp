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
    REQUIRE (cat["total"].get<int>() == 3);
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
