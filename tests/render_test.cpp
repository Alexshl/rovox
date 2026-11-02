// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Limiter.h"
#include "Loudness.h"
#include "Render.h"
#include "Sha256.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <chrono>
#include <fstream>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

using namespace rovox;

namespace
{
/** Project with one mono sine track (imported through the command layer). */
std::string sineProject (test::Env& env, double dbfs, double seconds, double hz = 1000.0, double sr = 44100.0)
{
    const auto id = env.newProject ("render");
    const auto dir = test::makeTempDir ("sine");
    test::writeWav (dir / "sine.wav", { test::sine (hz, dbfs, seconds, sr) }, sr, 24);
    auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "sine.wav").string() } } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    return id;
}

RenderRequest plainRequest()
{
    RenderRequest r;
    r.tailS = 0.0;
    return r;
}
} // namespace

ROVOX_TEST (render, sine_minus_20_dbfs_has_expected_loudness_and_peak)
{
    test::Env env;
    const auto id = sineProject (env, -20.0, 10.0);
    auto result = renderProject (env.project (id), plainRequest());
    REQUIRE (result.ok());
    const auto& r = result.value();
    CHECK (r["status"] == "succeeded");

    // BS.1770 reference: the same material, mono sine duplicated to L/R, analysed independently of the render
    const auto ref = test::sine (1000.0, -20.0, 10.0, 44100.0);
    const auto expected = analyse ({ ref, ref }, 44100.0);
    const auto got = r["stats"];
    std::cerr << "    rendered integrated " << got["integrated_lufs"] << " LUFS, true peak " << got["true_peak_dbtp"] << " dBTP; unity L=R reference "
              << expected.integratedLufs << " LUFS / " << expected.truePeakDbtp << " dBTP\n";

    // a -20 dBFS (peak) 1 kHz sine on both channels is -20 LUFS; Tracktion's centre pan law takes 3 dB off a mono source
    const double panLawDb = got["integrated_lufs"].get<double>() - expected.integratedLufs;
    CHECK (std::abs (panLawDb) < 0.1 || std::abs (panLawDb + 3.0) < 0.15);
    CHECK_NEAR (got["true_peak_dbtp"].get<double>(), expected.truePeakDbtp + panLawDb, 0.2);
    CHECK_NEAR (got["duration_s"].get<double>(), 10.0, 1e-6);
    CHECK (got["clipped_samples"] == 0);
    CHECK (got["silence_ranges"].empty());

    CHECK (r["source_revision"] == env.project (id).revision());
    CHECK (r["file"]["sha256"].get<std::string>().size() == 64);
    CHECK (fs::exists (r["file"]["abs_path"].get<std::string>()));
    CHECK (r["file"]["path"] == "renders/mix.wav");
    CHECK (r["metadata"]["engine"]["tracktion"].is_string());
    CHECK (r["metadata"]["input_hash"].get<std::string>().size() == 64);
    CHECK (fs::exists (env.project (id).rendersDir() / "mix.render.json"));
}

ROVOX_TEST (render, same_name_overwrites_the_same_path_names_with_spaces_and_cyrillic_work)
{
    test::Env env;
    const auto id = sineProject (env, -20.0, 2.0);
    auto req = plainRequest();
    req.name = "Рахів майстер 01";
    auto a = renderProject (env.project (id), req);
    REQUIRE (a.ok());
    auto b = renderProject (env.project (id), req);
    REQUIRE (b.ok());
    CHECK (a.value()["file"]["path"] == b.value()["file"]["path"]);
    CHECK (a.value()["file"]["path"] == "renders/Рахів майстер 01.wav");

    int files = 0;

    for (auto& e : fs::directory_iterator (env.project (id).rendersDir()))
        files += e.path().extension() == ".wav" ? 1 : 0;

    CHECK (files == 1);                // no -001 increments, no leftover temp files

    for (auto& e : fs::directory_iterator (env.project (id).rendersDir()))
        CHECK (e.path().filename().string().rfind (".", 0) != 0);
}

ROVOX_TEST (render, deleted_media_fails_preflight_and_writes_nothing)
{
    test::Env env;
    const auto id = sineProject (env, -20.0, 2.0);
    auto& p = env.project (id);
    fs::remove (p.dir() / p.manifest().media[0].path);

    auto result = renderProject (p, plainRequest());
    REQUIRE (! result.ok());
    CHECK (result.error().code == errc::preflightFailed);
    REQUIRE (! result.error().details["details"].empty());
    CHECK (result.error().details["details"][0]["check"] == "offline_media");
    CHECK (! result.error().details["details"][0]["track_id"].get<std::string>().empty());
    CHECK (! fs::exists (p.rendersDir() / "mix.wav"));
}

ROVOX_TEST (render, plugin_that_failed_to_load_is_not_a_silent_bypass)
{
    test::Env env;
    const auto id = sineProject (env, -20.0, 2.0);
    const auto trackId = env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();
    auto added = env.run (id, "fx.add", { { "track_id", trackId }, { "plugin", { { "format", "LV2" }, { "uid", "urn:rovox:does-not-exist" }, { "name", "Ghost EQ" } } } });
    REQUIRE (added.ok);
    CHECK (added.data["plugin"]["loaded"] == false);

    auto result = renderProject (env.project (id), plainRequest());
    REQUIRE (! result.ok());
    CHECK (result.error().code == errc::preflightFailed);
    CHECK (result.error().details["details"][0]["check"] == "plugin_not_loaded");
    CHECK (result.error().details["details"][0]["name"] != nullptr);
    CHECK (! fs::exists (env.project (id).rendersDir() / "mix.wav"));

    // asking to only warn lets the render through, with the finding in the report
    auto req = plainRequest();
    req.failOn.erase ("plugin_not_loaded");
    req.warnOn.insert ("plugin_not_loaded");
    auto warned = renderProject (env.project (id), req);
    REQUIRE (warned.ok());
    CHECK (warned.value()["preflight"]["warnings"][0]["check"] == "plugin_not_loaded");
}

ROVOX_TEST (render, midi_track_with_silent_instrument_is_reported)
{
    test::Env env;
    const auto id = env.newProject ("midi");
    auto& p = env.project (id);
    auto& edit = p.edit();

    auto addMidiTrack = [&] (const char* name, bool withInstrument, float volumeDb)
    {
        auto track = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);
        track->setName (name);
        track->getVolumePlugin()->setVolumeDb (volumeDb);

        if (withInstrument)
        {
            auto plugin = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});
            track->pluginList.insertPlugin (plugin, 0, nullptr);
        }

        auto clip = te::insertMIDIClip (*track, name, { te::TimePosition::fromSeconds (0.0), te::TimeDuration::fromSeconds (4.0) });
        clip->getSequence().addNote (60, te::BeatPosition::fromBeats (0.0), te::BeatDuration::fromBeats (2.0), 100, 0, nullptr);
        return track;
    };

    addMidiTrack ("Loud synth", true, 0.0f);
    addMidiTrack ("No instrument", false, 0.0f);
    test::pumpMessages (50);

    auto result = renderProject (p, plainRequest());
    REQUIRE (! result.ok());
    CHECK (result.error().code == errc::preflightFailed);

    bool noInstrument = false, loudFlagged = false;

    for (auto& f : result.error().details["details"])
    {
        noInstrument = noInstrument || (f["check"] == "silent_track" && f["name"] == "No instrument");
        loudFlagged = loudFlagged || f["name"] == "Loud synth";
    }

    CHECK (noInstrument);
    CHECK (! loudFlagged);

    // a synth that is turned all the way down is silent too
    auto tracks = te::getAudioTracks (edit);
    tracks[1]->setMute (true);                                  // drop the instrument-less one
    tracks[0]->getVolumePlugin()->setVolumeDb (-100.0f);
    test::pumpMessages (50);
    auto quiet = renderProject (p, plainRequest());
    REQUIRE (! quiet.ok());
    CHECK (quiet.error().details["details"][0]["check"] == "silent_track");

    // and with the fader up the same project renders, and the probe reports it as audible
    tracks[0]->getVolumePlugin()->setVolumeDb (0.0f);
    test::pumpMessages (50);
    auto fine = renderProject (p, plainRequest());

    if (! fine.ok())
        std::cerr << "    fine: " << fine.error().toJson().dump() << "\n";

    REQUIRE (fine.ok());
    CHECK (fine.value()["tracks"][0]["audible"] == true);
    CHECK (fine.value()["stats"]["sample_peak_dbfs"].get<double>() > -60.0);
}

ROVOX_TEST (render, silent_project_fails_post_check)
{
    test::Env env;
    const auto id = sineProject (env, -90.0, 2.0);
    auto result = renderProject (env.project (id), plainRequest());
    REQUIRE (! result.ok());
    CHECK (result.error().code == errc::preflightFailed);
    CHECK (result.error().details["details"][0]["check"] == "silent_output");
    CHECK (! fs::exists (env.project (id).rendersDir() / "mix.wav"));
}

ROVOX_TEST (render, clipping_is_detected_in_float_before_conversion)
{
    test::Env env;
    const auto id = sineProject (env, -0.5, 1.0);
    const auto trackId = env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();
    env.ok (id, "track.update", { { "track_id", trackId }, { "volume_db", 12.0 } });
    auto result = renderProject (env.project (id), plainRequest());
    REQUIRE (! result.ok());
    CHECK (result.error().details["details"][0]["check"] == "clipping");
}

ROVOX_TEST (render, normalize_to_target_lufs_reports_before_and_after)
{
    test::Env env;
    const auto id = sineProject (env, -30.0, 5.0);
    auto req = plainRequest();
    req.loudness.mode = "normalize";
    req.loudness.targetLufs = -16.0;
    req.loudness.toleranceLu = 0.2;
    auto result = renderProject (env.project (id), req);
    REQUIRE (result.ok());
    const auto& l = result.value()["loudness"];
    CHECK_NEAR (l["after_lufs"].get<double>(), -16.0, 0.2);
    CHECK (l["before_lufs"].get<double>() < -29.0);
    CHECK (l["gain_db"].get<double>() > 10.0);
    CHECK (l["on_target"] == true);
    CHECK_NEAR (result.value()["stats"]["integrated_lufs"].get<double>(), -16.0, 0.2);
    CHECK (result.value().contains ("stats_before_gain"));

    // check mode: a missed target fails the render
    auto checkReq = plainRequest();
    checkReq.loudness.mode = "check";
    checkReq.loudness.targetLufs = -11.0;
    auto missed = renderProject (env.project (id), checkReq);
    REQUIRE (! missed.ok());
    CHECK (missed.error().details["details"][0]["check"] == "loudness_target");

    // the true-peak ceiling wins over the target
    auto hot = plainRequest();
    hot.loudness.mode = "normalize";
    hot.loudness.targetLufs = -0.5;
    hot.loudness.truePeakCeilingDb = -1.0;
    hot.loudness.toleranceLu = 100.0;
    auto limited = renderProject (env.project (id), hot);
    REQUIRE (limited.ok());
    CHECK (limited.value()["loudness"]["limited_by_true_peak"] == true);
    CHECK (limited.value()["stats"]["true_peak_dbtp"].get<double>() <= -0.9);
}

ROVOX_TEST (render, formats_wav16_flac_mp3_and_mono)
{
    test::Env env;
    const auto id = sineProject (env, -12.0, 3.0);

    for (const char* container : { "wav", "flac", "mp3" })
    {
        auto req = plainRequest();
        req.name = std::string ("fmt_") + container;
        req.format.container = container;
        req.format.bitDepth = 16;
        auto r = renderProject (env.project (id), req);
        REQUIRE (r.ok());
        const auto path = r.value()["file"]["abs_path"].get<std::string>();
        CHECK (fs::file_size (path) > 1000);
        CHECK_NEAR (r.value()["stats"]["duration_s"].get<double>(), 3.0, 0.01);
        CHECK (path.substr (path.size() - std::string (container).size()) == container);

        if (std::string (container) != "mp3")
        {
            std::vector<std::vector<float>> data;
            double sr = 0;
            REQUIRE (test::readWav (path, data, sr));
            CHECK (sr == 44100.0);
            CHECK (data.size() == 2);
            CHECK (data[0].size() == 132300);
        }
    }

    auto mono = plainRequest();
    mono.name = "mono";
    mono.format.channels = 1;
    auto m = renderProject (env.project (id), mono);
    REQUIRE (m.ok());
    std::vector<std::vector<float>> data;
    double sr = 0;
    REQUIRE (test::readWav (m.value()["file"]["abs_path"].get<std::string>(), data, sr));
    CHECK (data.size() == 1);
}

ROVOX_TEST (render, range_and_stems)
{
    test::Env env;
    const auto id = sineProject (env, -12.0, 6.0);
    env.ok (id, "track.create", { { "name", "Empty" } });
    const auto tracks = env.ok (id, "track.list")["tracks"];

    auto req = plainRequest();
    req.range.startS = 1.0;
    req.range.endS = 3.5;
    auto r = renderProject (env.project (id), req);
    REQUIRE (r.ok());
    CHECK_NEAR (r.value()["stats"]["duration_s"].get<double>(), 2.5, 1e-4);

    auto stems = plainRequest();
    stems.scope = "stems";
    stems.trackIds = { tracks[0]["track_id"].get<std::string>() };
    stems.name = "stem";
    auto s = renderProject (env.project (id), stems);
    REQUIRE (s.ok());
    REQUIRE (s.value()["files"].size() == 1);
    CHECK (s.value()["files"][0]["path"].get<std::string>().find ("stem.01-") != std::string::npos);

    auto unknown = plainRequest();
    unknown.scope = "stems";
    unknown.trackIds = { "trk_missing" };
    CHECK (! renderProject (env.project (id), unknown).ok());
}

ROVOX_TEST (render, empty_project_is_a_precondition_error)
{
    test::Env env;
    const auto id = env.newProject();
    auto r = renderProject (env.project (id), plainRequest());
    REQUIRE (! r.ok());
    CHECK (r.error().code == errc::preconditionFailed);
}

ROVOX_TEST (render, change_after_snapshot_does_not_reach_the_file)
{
    test::Env env;
    const auto id = sineProject (env, -20.0, 2.0);
    auto& p = env.project (id);
    auto snapshot = makeRenderSnapshot (p);
    const auto revision = p.revision();

    // the live project moves on after the snapshot was taken
    env.ok (id, "track.update", { { "track_id", env.ok (id, "track.list")["tracks"][0]["track_id"] }, { "volume_db", -40.0 } });
    CHECK (p.revision() == revision + 1);

    RenderContext ctx;
    ctx.rendersDir = p.rendersDir();
    ctx.projectId = id;
    ctx.sourceRevision = revision;
    ctx.projectSampleRate = 44100.0;
    auto r = renderEdit (*snapshot, ctx, plainRequest());
    REQUIRE (r.ok());
    CHECK (r.value()["source_revision"] == revision);
    CHECK (r.value()["stats"]["integrated_lufs"].get<double>() > -30.0);        // still the unmodified level
}

ROVOX_TEST (render, deterministic_input_hash_and_identical_float_output_on_one_architecture)
{
    test::Env env;
    const auto id = sineProject (env, -20.0, 3.0);
    auto req = plainRequest();
    req.format.bitDepth = 32;
    req.seed = "s1";
    auto a = renderProject (env.project (id), req);
    REQUIRE (a.ok());
    const auto shaA = a.value()["file"]["sha256"].get<std::string>();
    auto b = renderProject (env.project (id), req);
    REQUIRE (b.ok());
    CHECK (b.value()["file"]["sha256"] == shaA);
    CHECK (b.value()["metadata"]["input_hash"] == a.value()["metadata"]["input_hash"]);
    CHECK (b.value()["metadata"]["seed"] == "s1");

    req.seed = "s2";
    auto c = renderProject (env.project (id), req);
    REQUIRE (c.ok());
    CHECK (c.value()["metadata"]["input_hash"] != a.value()["metadata"]["input_hash"]);
}

// ---- the 60.000 s hang (spike P5) ---------------------------------------------------------------------------

namespace rovox::test
{
int runChildMode (int argc, char** argv)
{
    // usage: --child raw-render <seconds> <workdir> [workaround]
    if (argc >= 5 && (std::string (argv[2]) == "raw-render" || std::string (argv[2]) == "raw-render-fx"))
    {
        const double seconds = std::stod (argv[3]);
        const fs::path work = argv[4];
        const bool workaround = argc >= 6;
        fs::create_directories (work);
        Env env;
        const auto id = env.newProject ("child");
        const bool fx = std::string (argv[2]) == "raw-render-fx";
        json paths = json::array();

        for (int i = 1; i <= (fx ? 8 : 1); ++i)
        {
            const auto f = work / ("tone" + std::to_string (i) + ".wav");
            writeWav (f, { sine (220.0 * i, -20, seconds + 5.0, 44100.0) }, 44100.0, 24);
            paths.push_back (f.string());
        }

        auto plan = env.ok (id, "import.plan", { { "paths", paths } });
        env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });

        if (fx)
        {
            // the spike P5 scenario: 8 tracks with EQ + compressor and a compressor on the master
            for (auto& t : env.ok (id, "track.list")["tracks"])
                for (const char* kind : { "equaliser", "compressor" })
                    env.ok (id, "fx.add", { { "track_id", t["track_id"] }, { "builtin", kind } });

            env.ok (id, "fx.add", { { "track_id", "master" }, { "builtin", "compressor" } });
        }

        auto snapshot = makeRenderSnapshot (env.project (id));
        std::cerr << "child: rendering " << seconds << " s" << (workaround ? " with workaround" : " raw") << "\n";
        auto r = renderRawWav (*snapshot, work / "out.wav", { te::TimePosition(), te::TimePosition::fromSeconds (seconds) }, 0.0, 44100.0, {}, true, 1000.0, workaround);
        std::cerr << "child: " << (r.ok() ? "finished" : r.error().message) << "\n";
        return r.ok() ? 0 : 1;
    }

    return 2;
}
}

namespace
{
/** Runs this binary as `--child ...` and waits at most `timeoutS`; returns the exit status or -1 on timeout (child killed). */
int runChild (const std::vector<std::string>& args, double timeoutS, const char* cpus = nullptr)
{
    const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    const pid_t pid = fork();

    if (pid == 0)
    {
        if (cpus != nullptr)
            setenv ("ROVOX_CPUS", cpus, 1);

        std::vector<char*> argv;
        argv.push_back (const_cast<char*> (self.c_str()));
        argv.push_back (const_cast<char*> ("--child"));

        for (auto& a : args)
            argv.push_back (const_cast<char*> (a.c_str()));

        argv.push_back (nullptr);
        execv (self.c_str(), argv.data());
        _exit (127);
    }

    const auto t0 = std::chrono::steady_clock::now();

    for (;;)
    {
        int status = 0;

        if (waitpid (pid, &status, WNOHANG) == pid)
            return WIFEXITED (status) ? WEXITSTATUS (status) : 128;

        if (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > timeoutS)
        {
            kill (pid, SIGKILL);
            waitpid (pid, &status, 0);
            return -1;
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
}
} // namespace

ROVOX_TEST (render, sixty_second_render_hang_reproduction)
{
    // Reproduction of the spike P5 finding, in a child process so that a hang cannot take the suite down.
    // Passing does not depend on whether this Tracktion build still hangs: the point is the recorded outcome.
    const auto work = test::makeTempDir ("sixty").string();
    const int raw = runChild ({ "raw-render", "60.0", work + "/raw" }, 45.0);
    std::cerr << "    raw Tracktion render of exactly 60.000 s: " << (raw == -1 ? "HUNG (killed after 45 s)" : raw == 0 ? "completed (hang not reproduced)" : "failed") << "\n";

    for (const char* cpus : { "1", "2", "4" })
    {
        const int r = runChild ({ "raw-render", "60.0", work + "/raw-cpus" + cpus }, 45.0, cpus);
        std::cerr << "    raw render of exactly 60.000 s with " << cpus << " engine cpu(s): " << (r == -1 ? "HUNG (killed after 45 s)" : r == 0 ? "completed" : "failed") << "\n";
    }

    const int fxRaw = runChild ({ "raw-render-fx", "60.0", work + "/rawfx" }, 60.0);
    std::cerr << "    raw render of exactly 60.000 s, 8 tracks with EQ + compressor (spike P5 scenario): "
              << (fxRaw == -1 ? "HUNG (killed after 60 s)" : fxRaw == 0 ? "completed" : "failed") << "\n";
    const int fxFixed = runChild ({ "raw-render-fx", "60.0", work + "/fixfx", "workaround" }, 60.0);
    CHECK (fxFixed == 0);

    const int sane = runChild ({ "raw-render", "59.0", work + "/59" }, 45.0);
    CHECK (sane == 0);                      // 59 s is fine, so a hang at 60 s is specific to the length

    const int worked = runChild ({ "raw-render", "60.0", work + "/fixed", "workaround" }, 45.0);
    CHECK (worked == 0);                    // the workaround must always complete
}

ROVOX_TEST (render, exactly_sixty_seconds_renders_with_the_workaround_and_is_sample_exact)
{
    test::Env env;
    const auto id = env.newProject ("sixty");
    const auto dir = test::makeTempDir ("sixty-src");
    test::writeWav (dir / "tone.wav", { test::sine (440, -20, 62.0, 44100.0) }, 44100.0, 24);
    auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "tone.wav").string() } } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });

    auto req = plainRequest();
    req.range.startS = 0.0;
    req.range.endS = 60.0;
    req.stallTimeoutS = 30;
    req.format.bitDepth = 32;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = renderProject (env.project (id), req);
    const auto secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    REQUIRE (r.ok());
    CHECK (secs < 30.0);
    std::vector<std::vector<float>> data;
    double sr = 0;
    REQUIRE (test::readWav (r.value()["file"]["abs_path"].get<std::string>(), data, sr));
    CHECK (data[0].size() == 60 * 44100);
    CHECK (needsSixtySecondWorkaround (60.0));
    CHECK (! needsSixtySecondWorkaround (59.0));
}

//==============================================================================
// Master limiter (true-peak brickwall) and the normalize + limiter flow
namespace
{
/** Sustained sine at -24 dBFS plus short decaying bursts (transients) every 0.4 s at -2 dBFS. */
std::vector<float> peakySignal (double seconds, double sr, double burstPhase)
{
    auto x = test::sine (220.0, -24.0, seconds, sr);
    const auto period = (size_t) (0.4 * sr);

    for (size_t start = (size_t) (0.1 * sr); start < x.size(); start += period)
        for (size_t i = 0; i < (size_t) (0.006 * sr) && start + i < x.size(); ++i)
            x[start + i] += (float) (0.79 * std::exp (-(double) i / (0.0015 * sr)) * std::sin (2.0 * 3.14159265358979 * 3100.0 * (double) i / sr + burstPhase));

    return x;
}
} // namespace

ROVOX_TEST (render, limiter_brings_peaky_mix_to_target_with_true_peak_under_ceiling)
{
    test::Env env;
    const auto id = env.newProject ("render");
    const auto dir = test::makeTempDir ("peaky");
    test::writeWav (dir / "peaky.wav", { peakySignal (12.0, 44100.0, 0.0), peakySignal (12.0, 44100.0, 0.7) }, 44100.0, 24);
    auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "peaky.wav").string() } } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });

    for (const char* container : { "wav", "flac", "mp3" })
    {
        auto req = plainRequest();
        req.format.container = container;
        req.loudness.mode = "normalize";
        req.loudness.targetLufs = -14.0;
        req.loudness.toleranceLu = 0.3;
        auto result = renderProject (env.project (id), req);
        REQUIRE (result.ok());
        const auto& l = result.value()["loudness"];
        CHECK_NEAR (l["after_lufs"].get<double>(), -14.0, 0.3);
        CHECK (result.value()["stats"]["true_peak_dbtp"].get<double>() <= -1.0 + 0.05);
        REQUIRE (l.contains ("limiter"));
        CHECK (l["limiter"]["max_gain_reduction_db"].get<double>() > 1.0);
        CHECK (l["limiter"]["passes"].get<int>() <= 4);
    }

    // limiter off: the old behaviour, gain only and held back by the ceiling
    auto off = plainRequest();
    off.loudness.mode = "normalize";
    off.loudness.targetLufs = -14.0;
    off.loudness.toleranceLu = 100.0;
    off.loudness.limiter = false;
    auto result = renderProject (env.project (id), off);
    REQUIRE (result.ok());
    const auto& l = result.value()["loudness"];
    CHECK (! l.contains ("limiter"));
    CHECK (l["limited_by_true_peak"] == true);
    CHECK (l["after_lufs"].get<double>() < -14.5);
    CHECK (result.value()["stats"]["true_peak_dbtp"].get<double>() <= -0.9);
}

ROVOX_TEST (render, limiter_unit_silence_stays_silence_and_stereo_is_linked)
{
    const double sr = 48000.0;
    std::vector<std::vector<float>> silent (2, std::vector<float> (48000, 0.0f));
    auto r = limitTruePeak (silent, sr, 12.0);
    CHECK (r.maxGainReductionDb == 0.0);

    for (auto& c : silent)
        for (float v : c)
            CHECK (v == 0.0f);

    // left carries transients, right a steady quiet sine: both get the same gain
    std::vector<std::vector<float>> in { peakySignal (2.0, sr, 0.0), test::sine (440.0, -30.0, 2.0, sr) };
    auto out = in;
    const auto lr = limitTruePeak (out, sr, 6.0, { -1.0, 1.5, 80.0 });
    CHECK (lr.maxGainReductionDb > 1.0);
    const auto stats = analyse (out, sr);
    CHECK (stats.truePeakDbtp <= -1.0 + 0.05);
    size_t checked = 0;

    for (size_t i = 0; i < in[0].size(); ++i)
    {
        if (std::abs (in[0][i]) > 0.05f && std::abs (in[1][i]) > 0.01f)
        {
            const double gl = out[0][i] / in[0][i], gr = out[1][i] / in[1][i];
            CHECK_NEAR (gl, gr, 1.0e-4);
            ++checked;
        }
    }

    CHECK (checked > 1000);
    CHECK (out[1].size() == in[1].size());
}

ROVOX_TEST (render, limiter_unit_quiet_signal_below_ceiling_only_gets_the_gain)
{
    const double sr = 44100.0;
    std::vector<std::vector<float>> x { test::sine (1000.0, -30.0, 1.0, sr) };
    auto y = x;
    const auto r = limitTruePeak (y, sr, 6.0);
    CHECK (r.maxGainReductionDb == 0.0);
    CHECK_NEAR (y[0][1000] / x[0][1000], std::pow (10.0, 6.0 / 20.0), 1.0e-4);
}
