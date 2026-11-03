// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// M4: plugin catalog and probe, hosting outside the server process (a crashing plugin must not kill the server), MIDI tools played by
// an LV2 instrument, sends, track / item duplication, presets, plugin moves, templates, project catalog. Every mutating command is
// undone and redone. Needs the LV2 plugins of the image (zynaddsubfx, calf, lsp) and the fixtures built by tests/CMakeLists.txt.
#include "EventHub.h"
#include "JobManager.h"
#include "Loudness.h"
#include "PluginCatalog.h"
#include "PluginHost.h"
#include "RenderJobs.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

using namespace rovox;

namespace
{
const char* crashEarly = "lv2:urn:rovox:test:crash-early";
const char* crashLate = "lv2:urn:rovox:test:crash-late";
const char* passThrough = "lv2:urn:rovox:test:pass";

bool requireImagePlugins()
{
    return std::getenv ("ROVOX_REQUIRE_LV2") != nullptr;
}

/** True if the catalog has what the test needs; fails the test when the image is supposed to have it. */
bool have (const json& catalog, const char* what)
{
    if (! catalog["plugins"].empty())
        return true;

    if (requireImagePlugins())
    {
        std::cerr << "    the image should have a plugin matching '" << what << "' but the catalog has none\n";
        CHECK (false);
    }
    else
    {
        std::cerr << "    skipped: no plugin matching '" << what << "' installed\n";
    }

    return false;
}

std::string pluginUid (test::Env& env, const std::string& kind, const std::string& query)
{
    auto r = env.run ("", "plugin.catalog", { { "kind", kind }, { "query", query }, { "limit", 1 } });
    REQUIRE (r.ok);
    return have (r.data, query.c_str()) ? r.data["plugins"][0]["plugin_uid"].get<std::string>() : std::string();
}

json catalogAll (test::Env& env)
{
    return env.run ("", "plugin.catalog", { { "limit", 500 } }).data;
}

void scanOnce (test::Env& env)
{
    static bool done = false;

    if (! done)
    {
        auto r = env.run ("", "plugin.rescan", { { "force", true } });
        REQUIRE (r.ok);
        done = true;
    }
}

std::string sineTrackProject (test::Env& env, double seconds = 8.0)
{
    const auto id = env.newProject ("plugins");
    const auto dir = test::makeTempDir ("plugin-sine");
    test::writeWav (dir / "sine.wav", { test::sine (440.0, -20.0, seconds, 44100.0) }, 44100.0, 24);
    auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "sine.wav").string() } } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    return id;
}

std::string firstTrack (test::Env& env, const std::string& id)
{
    return env.ok (id, "track.list")["tracks"][0]["track_id"].get<std::string>();
}

json waitForJob (JobManager& jobs, const std::string& id, double seconds = 90)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double> (seconds);

    for (;;)
    {
        test::pumpMessages (10);
        auto j = jobs.get (id);

        if (j && isFinished (j->state))
            return j->toJson();

        REQUIRE (std::chrono::steady_clock::now() < deadline);
    }
}

double peakOf (const fs::path& wav)
{
    std::vector<std::vector<float>> ch;
    double sr = 0, peak = 0;
    REQUIRE (test::readWav (wav, ch, sr));

    for (auto& c : ch)
        for (float s : c)
            peak = std::max (peak, (double) std::abs (s));

    return peak;
}

json notes (std::initializer_list<std::pair<int, double>> pitchAndStart, double length = 0.5)
{
    json out = json::array();

    for (auto& [pitch, start] : pitchAndStart)
        out.push_back ({ { "pitch", pitch }, { "start_qn", start }, { "length_qn", length }, { "velocity", 100 } });

    return out;
}
} // namespace

//==============================================================================
ROVOX_TEST (plugins, scan_builds_a_catalog_and_the_next_scan_is_incremental)
{
    test::Env env;
    auto t0 = std::chrono::steady_clock::now();
    auto first = env.run ("", "plugin.rescan", { { "force", true } });
    REQUIRE (first.ok);
    CHECK (first.data["unchanged"] == false);
    CHECK (first.data["plugins"].get<int>() >= 3);

    auto second = env.run ("", "plugin.rescan");
    REQUIRE (second.ok);
    CHECK (second.data["unchanged"] == true);
    CHECK (second.data["elapsed_ms"].get<double>() < 2000.0);
    std::cerr << "    full scan " << std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() << " s, unchanged rescan "
              << second.data["elapsed_ms"] << " ms\n";

    auto beep = env.run ("", "plugin.catalog", { { "query", "Rovox Test Beep" } });
    REQUIRE (beep.ok);
    REQUIRE (beep.data["total"] == 1);
    CHECK (beep.data["plugins"][0]["kind"] == "instrument");

    auto fixtures = env.run ("", "plugin.catalog", { { "query", "Rovox Test" }, { "kind", "effect" } });
    REQUIRE (fixtures.ok);
    CHECK (fixtures.data["total"] == 3);
    CHECK (fixtures.data["plugins"][0]["connector"] == "local");
    CHECK (fixtures.data["plugins"][0]["kind"] == "effect");
    CHECK (fixtures.data["plugins"][0]["status"] == "ok");
    CHECK (fixtures.data["plugins"][0].contains ("desc_xml") == false);
    CHECK (fixtures.data["scan"]["state"] == "done");
    CHECK (fixtures.data["connectors"].size() == 3);

    // the catalog survives a restart: a second catalog object on the same directory reads the file
    PluginCatalog again (env.pm.host());
    CHECK (again.size() == env.pm.host().catalog().size());

    auto tagged = env.run ("", "plugin.tag", { { "plugin_uid", passThrough }, { "tags", { "test", "utility" } } });
    REQUIRE (tagged.ok);
    CHECK (env.run ("", "plugin.catalog", { { "tag", "utility" } }).data["total"] == 1);
}

ROVOX_TEST (plugins, instruments_in_the_image_are_listed_with_presets)
{
    test::Env env;
    scanOnce (env);
    auto r = env.run ("", "plugin.catalog", { { "kind", "instrument" }, { "query", "ZynAddSubFX" }, { "include_presets", true } });
    REQUIRE (r.ok);

    if (! have (r.data, "ZynAddSubFX"))
        return;

    auto& zyn = r.data["plugins"][0];
    CHECK (zyn["kind"] == "instrument");
    CHECK (zyn["io"]["midi_in"] == true);
    CHECK (zyn.contains ("presets"));
    std::cerr << "    ZynAddSubFX: " << zyn["presets"].size() << " presets\n";

    auto missing = env.run ("", "plugin.catalog", { { "query", "no-such-plugin-xyz" } });
    CHECK (missing.data["plugins"].empty());
}

ROVOX_TEST (plugins, a_plugin_that_crashes_is_blacklisted_and_the_server_lives)
{
    test::Env env;
    scanOnce (env);
    const auto id = sineTrackProject (env);
    const auto track = firstTrack (env, id);
    const auto chainBefore = env.ok (id, "fx.list", { { "target", track } })["fx"].size();

    // crash-early aborts on its 3rd block: the child process that probes it dies, this process does not
    auto added = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", crashEarly } } } });
    REQUIRE (! added.ok);
    CHECK (added.error->code == errc::pluginCrashed);
    std::cerr << "    " << added.error->message << "\n";

    auto listed = env.run ("", "plugin.catalog", { { "query", "Crash Early" } });
    CHECK (listed.data["plugins"][0]["status"] == "blacklisted");

    auto again = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", crashEarly } } } });
    REQUIRE (! again.ok);
    CHECK (again.error->code == errc::pluginBlacklisted);
    CHECK (env.run ("", "plugin.catalog", { { "status", "blacklisted" } }).data["total"] == 1);

    // the project is untouched and keeps working
    CHECK (env.ok (id, "fx.list", { { "target", track } })["fx"].size() == chainBefore);

    env.ok ("", "plugin.unblacklist", { { "plugin_uid", crashEarly } });
    CHECK (env.run ("", "plugin.catalog", { { "status", "blacklisted" } }).data["total"] == 0);
}

ROVOX_TEST (plugins, a_plugin_that_passes_the_probe_loads_and_reports_latency)
{
    test::Env env;
    scanOnce (env);
    auto probe = env.run ("", "plugin.probe", { { "plugin_uid", passThrough } });
    REQUIRE (probe.ok);
    CHECK (probe.data["probe"]["inputs"] == 1);
    CHECK (probe.data["probe"]["latency_samples"] == 0);

    const auto id = sineTrackProject (env);
    const auto chainBefore = env.ok (id, "fx.list", { { "target", firstTrack (env, id) } })["fx"].size();
    auto added = env.run (id, "fx.add", { { "track_id", firstTrack (env, id) }, { "plugin", { { "plugin_uid", passThrough } } } });
    REQUIRE (added.ok);
    CHECK (added.data["plugin"]["loaded"] == true);
    CHECK (added.data["plugin"]["is_instrument"] == false);

    // undo / redo of fx.add
    env.ok (id, "undo");
    CHECK (env.ok (id, "fx.list", { { "target", firstTrack (env, id) } })["fx"].size() == chainBefore);
    env.ok (id, "redo");
    CHECK (env.ok (id, "fx.list", { { "target", firstTrack (env, id) } })["fx"].size() == chainBefore + 1);
}

ROVOX_TEST (plugins, a_render_that_crashes_a_plugin_fails_the_job_but_not_the_server)
{
    test::Env env;
    scanOnce (env);
    const auto id = sineTrackProject (env, 6.0);
    const auto track = firstTrack (env, id);
    auto added = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", crashLate } } } });
    REQUIRE (added.ok);                                    // the probe (8 blocks) is fine, the 150th block is not
    const auto pluginId = added.data["plugin_id"].get<std::string>();

    std::atomic<int> crashEvents { 0 };
    const auto observer = env.hub.addObserver ([&] (const Event& e) { if (e.body.value ("type", "") == "plugin.crashed") ++crashEvents; });

    JobManager jobs { fs::path() };
    JobService service (env.pm, jobs, env.executor);
    JobContextInfo who { "local", id, Origin::system() };
    auto started = service.startRender (who, { { "name", "crashy" } }, false);
    REQUIRE (started.ok());
    auto job = waitForJob (jobs, started.value().jobId);
    CHECK (job["state"] == "failed");
    CHECK (job["error"]["code"] == errc::pluginCrashed);
    CHECK (job["error"]["message"].get<std::string>().find ("Rovox Test Crash Late") != std::string::npos);
    CHECK (! fs::exists (env.project (id).rendersDir() / "crashy.wav"));
    CHECK (crashEvents.load() == 1);
    std::cerr << "    " << job["error"]["message"].get<std::string>() << "\n";

    // a preview goes through the same guard
    auto preview = service.startPreview (who, { { "range", { { "start", { { "s", 0.0 } } }, { "end", { { "s", 5.0 } } } } } });
    REQUIRE (preview.ok());
    auto pj = waitForJob (jobs, preview.value().jobId);
    CHECK (pj["state"] == "failed");
    CHECK (pj["error"]["code"] == errc::pluginCrashed);

    // the server and the project are fine: other commands work, and removing the culprit makes the render succeed
    CHECK (env.ok (id, "project.info")["tracks"] == 1);
    env.ok (id, "fx.remove", { { "plugin_id", pluginId } });
    auto ok = service.startRender (who, { { "name", "fine" } }, false);
    REQUIRE (ok.ok());
    CHECK (waitForJob (jobs, ok.value().jobId)["state"] == "succeeded");
    CHECK (fs::exists (env.project (id).rendersDir() / "fine.wav"));

    // a project with a harmless external plugin renders in the child too, with the same result
    env.ok (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", passThrough } } } });
    auto viaChild = service.startRender (who, { { "name", "viachild" } }, false);
    REQUIRE (viaChild.ok());
    auto vj = waitForJob (jobs, viaChild.value().jobId);
    CHECK (vj["state"] == "succeeded");
    CHECK_NEAR (peakOf (env.project (id).rendersDir() / "viachild.wav"), peakOf (env.project (id).rendersDir() / "fine.wav"), 1e-4);

    env.hub.removeObserver (observer);
    jobs.shutdown();
}

ROVOX_TEST (plugins, midi_written_by_the_agent_is_played_by_an_lv2_instrument)
{
    test::Env env;
    scanOnce (env);
    const auto uid = pluginUid (env, "instrument", "ZynAddSubFX");

    if (uid.empty())
        return;

    const auto id = env.newProject ("synth");
    const auto track = env.ok (id, "track.create", { { "name", "Synth" } })["track_id"].get<std::string>();
    auto added = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", uid } } } });
    REQUIRE (added.ok);
    CHECK (added.data["plugin"]["is_instrument"] == true);
    CHECK (added.data["plugin"]["probe"]["audible"] == true);

    auto item = env.run (id, "item.create_midi", { { "track_id", track }, { "start", { { "bar", 1 } } }, { "name", "riff" },
                                                   { "notes", notes ({ { 48, 0.0 }, { 55, 1.0 }, { 60, 2.0 }, { 55, 3.0 } }, 0.9) } });
    REQUIRE (item.ok);
    const auto itemId = item.data["item_id"].get<std::string>();
    CHECK (item.data["item"]["notes"] == 4);
    CHECK (! item.data["item"].contains ("warning"));

    JobManager jobs { fs::path() };
    JobService service (env.pm, jobs, env.executor);
    JobContextInfo who { "local", id, Origin::system() };
    auto started = service.startRender (who, { { "name", "riff" }, { "check", true } }, false);
    REQUIRE (started.ok());
    auto job = waitForJob (jobs, started.value().jobId);

    if (job["state"] != "succeeded")
        std::cerr << "    " << job.dump() << "\n";

    REQUIRE (job["state"] == "succeeded");
    const double peak = peakOf (env.project (id).rendersDir() / "riff.wav");
    std::cerr << "    rendered riff peak " << peak << " (" << 20.0 * std::log10 (std::max (peak, 1e-9)) << " dBFS)\n";
    CHECK (peak > 0.01);

    // the same item without notes renders silence: the sound comes from the MIDI
    env.ok (id, "midi.write", { { "item_id", itemId }, { "notes", json::array() } });
    auto silent = service.startRender (who, { { "name", "none" } }, false);
    REQUIRE (silent.ok());
    waitForJob (jobs, silent.value().jobId);
    const auto silentPath = env.project (id).rendersDir() / "none.wav";

    if (fs::exists (silentPath))
        CHECK (peakOf (silentPath) < 0.001);

    // an item on a track without an instrument says so
    const auto bare = env.ok (id, "track.create", { { "name", "Bare" } })["track_id"].get<std::string>();
    auto bareItem = env.run (id, "item.create_midi", { { "track_id", bare }, { "notes", notes ({ { 60, 0.0 } }) } });
    REQUIRE (bareItem.ok);
    CHECK (bareItem.data["item"].contains ("warning"));
    jobs.shutdown();
}

ROVOX_TEST (plugins, an_instrument_plays_a_drum_pattern_in_the_child_render)
{
    // The instrument is the fixture "Rovox Test Beep" (deterministic, cannot crash): a third-party sample player (avldrums) segfaults in the
    // render child about every other run under load, which says nothing about Rovox. The crash path itself is covered by the crash-* fixtures.
    test::Env env;
    scanOnce (env);
    const auto id = env.newProject ("drums");
    const auto track = env.ok (id, "track.create", { { "name", "Drums" } })["track_id"].get<std::string>();
    auto added = env.run (id, "fx.add", { { "track_id", track }, { "plugin", { { "plugin_uid", "lv2:urn:rovox:test:beep" } } } });

    if (! added.ok)
        std::cerr << "    " << added.error->toJson().dump() << "\n";

    REQUIRE (added.ok);
    json pattern = json::array();

    for (int beat = 0; beat < 8; ++beat)
    {
        pattern.push_back ({ { "pitch", beat % 2 == 0 ? 36 : 38 }, { "start_qn", beat * 0.5 }, { "length_qn", 0.25 }, { "velocity", 110 } });
        pattern.push_back ({ { "pitch", 42 }, { "start_qn", beat * 0.5 }, { "length_qn", 0.1 }, { "velocity", 80 } });
    }

    REQUIRE (env.run (id, "item.create_midi", { { "track_id", track }, { "notes", pattern } }).ok);
    JobManager jobs { fs::path() };
    JobService service (env.pm, jobs, env.executor);
    auto started = service.startRender ({ "local", id, Origin::system() }, { { "name", "drums" }, { "check", true } }, false);
    REQUIRE (started.ok());
    auto job = waitForJob (jobs, started.value().jobId);

    if (job["state"] != "succeeded")
        std::cerr << "    " << job.dump() << "\n";

    REQUIRE (job["state"] == "succeeded");
    CHECK (peakOf (env.project (id).rendersDir() / "drums.wav") > 0.01);
    jobs.shutdown();
}

//==============================================================================
ROVOX_TEST (midi, create_write_edit_transpose_quantize_and_undo)
{
    test::Env env;
    const auto id = env.newProject ("midi");
    const auto track = env.ok (id, "track.create", { { "name", "Keys" } })["track_id"].get<std::string>();

    auto created = env.run (id, "item.create_midi", { { "track_id", track }, { "start", { { "bar", 2 } } }, { "length", { { "bars", 2 } } },
                                                      { "notes", json::array ({ { { "pitch", "C4" }, { "start_qn", 0.0 }, { "length_qn", 1.0 } }, { { "pitch", "F#3" }, { "start_qn", 1.1 }, { "length_qn", 1.0 }, { "velocity", 64 } } }) } });
    REQUIRE (created.ok);
    const auto item = created.data["item_id"].get<std::string>();
    CHECK_NEAR (created.data["item"]["start_s"].get<double>(), 2.0, 1e-6);          // bar 2 at 120 bpm 4/4
    CHECK_NEAR (created.data["item"]["length_qn"].get<double>(), 8.0, 1e-6);

    auto got = env.ok (id, "midi.get", { { "item_id", item } });
    REQUIRE (got["notes"].size() == 2);
    CHECK (got["notes"][0]["pitch"] == 60);
    CHECK (got["notes"][0]["name"] == "C4");
    CHECK (got["notes"][1]["pitch"] == 54);
    CHECK (got["notes"][1]["velocity"] == 64);
    const auto n0 = got["notes"][0]["note_id"].get<std::string>(), n1 = got["notes"][1]["note_id"].get<std::string>();
    CHECK (n0 != n1);

    // write: append keeps, replace swaps; the item grows to fit a late note
    auto appended = env.run (id, "midi.write", { { "item_id", item }, { "mode", "append" }, { "notes", notes ({ { 67, 9.0 } }) } });
    REQUIRE (appended.ok);
    CHECK (appended.data["item"]["notes"] == 3);
    CHECK_NEAR (appended.data["item"]["length_qn"].get<double>(), 12.0, 1e-6);
    env.ok (id, "undo");
    CHECK (env.ok (id, "midi.get", { { "item_id", item } })["notes"].size() == 2);
    CHECK_NEAR (env.ok (id, "midi.get", { { "item_id", item } })["item"]["length_qn"].get<double>(), 8.0, 1e-6);
    env.ok (id, "redo");
    CHECK (env.ok (id, "midi.get", { { "item_id", item } })["notes"].size() == 3);

    // edit: update, delete and add in one step
    auto edited = env.run (id, "midi.edit", { { "item_id", item }, { "update", json::array ({ { { "note_id", n0 }, { "pitch", "D4" }, { "velocity", 90 } } }) },
                                              { "delete", json::array ({ n1 }) }, { "add", notes ({ { 72, 4.0 } }) } });
    REQUIRE (edited.ok);
    CHECK (edited.data["deleted"].size() == 1);
    got = env.ok (id, "midi.get", { { "item_id", item } });
    CHECK (got["notes"].size() == 3);
    CHECK (got["notes"][0]["pitch"] == 62);
    CHECK (got["notes"][0]["velocity"] == 90);
    env.ok (id, "undo");
    CHECK (env.ok (id, "midi.get", { { "item_id", item } })["notes"][0]["pitch"] == 60);

    // transpose and its limits
    auto up = env.run (id, "midi.transpose", { { "item_id", item }, { "semitones", 12 } });
    REQUIRE (up.ok);
    CHECK (up.data["transposed"] == 3);
    CHECK (env.ok (id, "midi.get", { { "item_id", item } })["notes"][0]["pitch"] == 72);
    auto tooHigh = env.run (id, "midi.transpose", { { "item_id", item }, { "semitones", 60 } });
    CHECK (! tooHigh.ok);
    CHECK (tooHigh.error->code == errc::invalidParams);
    env.ok (id, "undo");
    CHECK (env.ok (id, "midi.get", { { "item_id", item } })["notes"][0]["pitch"] == 60);

    // quantize to eighth notes: 1.1 -> 1.0
    auto q = env.run (id, "midi.quantize", { { "item_id", item }, { "grid_qn", 0.5 } });
    REQUIRE (q.ok);
    CHECK (q.data["moved"] == 1);
    got = env.ok (id, "midi.get", { { "item_id", item } });
    CHECK_NEAR (got["notes"][1]["start_qn"].get<double>(), 1.0, 1e-6);
    env.ok (id, "undo");
    CHECK_NEAR (env.ok (id, "midi.get", { { "item_id", item } })["notes"][1]["start_qn"].get<double>(), 1.1, 1e-6);

    // validation: nothing is applied when one note is bad
    auto before = env.project (id).revision();
    auto bad = env.run (id, "midi.write", { { "item_id", item }, { "notes", json::array ({ { { "pitch", 60 }, { "start_qn", 0 }, { "length_qn", 1 } }, { { "pitch", "H9" }, { "start_qn", 1 }, { "length_qn", 1 } } }) } });
    CHECK (! bad.ok);
    CHECK (bad.error->code == errc::invalidParams);
    CHECK (env.project (id).revision() == before);
    CHECK (env.ok (id, "midi.get", { { "item_id", item } })["notes"].size() == 3);
    CHECK (! env.run (id, "midi.get", { { "item_id", "itm_nope" } }).ok);

    // the item shows up in item.list as MIDI and can be duplicated (undo removes the copy)
    auto dup = env.run (id, "item.duplicate", { { "item_id", item }, { "count", 2 } });
    REQUIRE (dup.ok);
    CHECK (dup.data["items"].size() == 2);
    CHECK_NEAR (dup.data["items"][0]["start_s"].get<double>(), 2.0 + 6.0, 1e-6);       // right after the original (it grew to 12 quarter notes = 6 s)
    CHECK_NEAR (dup.data["items"][1]["start_s"].get<double>(), 2.0 + 12.0, 1e-6);
    CHECK (env.ok (id, "midi.get", { { "item_id", dup.data["item_id"] } })["notes"].size() == 3);
    CHECK (env.ok (id, "item.list")["items"].size() == 3);
    env.ok (id, "undo");
    CHECK (env.ok (id, "item.list")["items"].size() == 1);
    env.ok (id, "redo");
    CHECK (env.ok (id, "item.list")["items"].size() == 3);

    // creating an item is undoable
    auto other = env.run (id, "item.create_midi", { { "track_id", track }, { "start_s", 30.0 } });
    REQUIRE (other.ok);
    env.ok (id, "undo");
    CHECK (! env.run (id, "midi.get", { { "item_id", other.data["item_id"] } }).ok);
}

ROVOX_TEST (midi, a_standard_midi_file_is_imported)
{
    test::Env env;
    const auto id = env.newProject ("smf");
    const auto track = env.ok (id, "track.create", { { "name", "Imported" } })["track_id"].get<std::string>();
    const auto dir = test::makeTempDir ("smf");

    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
    seq.addEvent (juce::MidiMessage::noteOff (1, 60), 480);
    seq.addEvent (juce::MidiMessage::noteOn (1, 64, 0.8f), 480);
    seq.addEvent (juce::MidiMessage::noteOff (1, 64), 1440);
    juce::MidiFile file;
    file.setTicksPerQuarterNote (480);
    file.addTrack (seq);
    juce::FileOutputStream out (toFile (dir / "riff.mid"));
    REQUIRE (out.openedOk());
    file.writeTo (out);
    out.flush();

    auto imported = env.run (id, "midi.import_file", { { "track_id", track }, { "file", (dir / "riff.mid").string() } });
    REQUIRE (imported.ok);
    CHECK (imported.data["imported_notes"] == 2);
    auto got = env.ok (id, "midi.get", { { "item_id", imported.data["item_id"] } });
    CHECK_NEAR (got["notes"][0]["length_qn"].get<double>(), 1.0, 1e-6);
    CHECK_NEAR (got["notes"][1]["start_qn"].get<double>(), 1.0, 1e-6);
    CHECK_NEAR (got["notes"][1]["length_qn"].get<double>(), 2.0, 1e-6);
    CHECK (! env.run (id, "midi.import_file", { { "track_id", track }, { "file", (dir / "nope.mid").string() } }).ok);
}

//==============================================================================
ROVOX_TEST (routing, a_send_feeds_a_reverb_bus_and_everything_is_undoable)
{
    test::Env env;
    const auto id = sineTrackProject (env, 3.0);
    const auto source = firstTrack (env, id);
    const auto bus = env.ok (id, "track.create", { { "name", "Reverb bus" } })["track_id"].get<std::string>();
    env.ok (id, "fx.add", { { "track_id", bus }, { "builtin", "reverb" } });

    auto created = env.run (id, "routing.send_create", { { "track_id", source }, { "to_track_id", bus }, { "gain_db", -6.0 } });
    REQUIRE (created.ok);
    const auto sendId = created.data["send_id"].get<std::string>();
    CHECK (sendId.rfind ("snd_", 0) == 0);
    CHECK (created.data["send"]["to_track_id"] == bus);
    CHECK_NEAR (created.data["send"]["gain_db"].get<double>(), -6.0, 0.01);
    CHECK (created.data["send"]["pre_fader"] == false);

    auto list = env.ok (id, "routing.list");
    REQUIRE (list["sends"].size() == 1);
    REQUIRE (list["returns"].size() == 1);
    CHECK (list["returns"][0]["track_id"] == bus);

    // a feedback loop is refused
    auto loop = env.run (id, "routing.send_create", { { "track_id", bus }, { "to_track_id", source } });
    CHECK (! loop.ok);
    CHECK (loop.error->code == errc::preconditionFailed);
    CHECK (! env.run (id, "routing.send_create", { { "track_id", source }, { "to_track_id", source } }).ok);

    auto updated = env.run (id, "routing.send_update", { { "send_id", sendId }, { "gain_db", -12.0 }, { "pre_fader", true } });
    REQUIRE (updated.ok);
    CHECK_NEAR (updated.data["send"]["gain_db"].get<double>(), -12.0, 0.01);
    CHECK (updated.data["send"]["pre_fader"] == true);
    env.ok (id, "undo");
    CHECK (env.ok (id, "routing.list")["sends"][0]["pre_fader"] == false);
    CHECK_NEAR (env.ok (id, "routing.list")["sends"][0]["gain_db"].get<double>(), -6.0, 0.01);

    // the bus makes the mix louder: render with the send, without (mute) and compare
    auto renderPeak = [&] (const char* name)
    {
        RenderRequest req;
        req.name = name;
        req.tailS = 0.0;
        auto r = renderProject (env.project (id), req);
        REQUIRE (r.ok());
        return r.value()["stats"]["integrated_lufs"].get<double>();
    };

    const double withSend = renderPeak ("with");
    env.ok (id, "routing.send_update", { { "send_id", sendId }, { "mute", true } });
    const double muted = renderPeak ("muted");
    std::cerr << "    mix with the send " << withSend << " LUFS, send muted " << muted << " LUFS\n";
    CHECK (withSend > muted + 0.05);
    env.ok (id, "undo");

    auto removed = env.run (id, "routing.send_delete", { { "send_id", sendId } });
    REQUIRE (removed.ok);
    CHECK (removed.data["return_removed"] == true);
    CHECK (env.ok (id, "routing.list")["sends"].empty());
    CHECK (env.ok (id, "routing.list")["returns"].empty());
    env.ok (id, "undo");
    CHECK (env.ok (id, "routing.list")["sends"].size() == 1);
    CHECK (env.ok (id, "routing.list")["returns"].size() == 1);
    CHECK (! env.run (id, "routing.send_delete", { { "send_id", "snd_nope" } }).ok);
}

ROVOX_TEST (routing, track_duplicate_copies_plugins_and_items_but_not_routing)
{
    test::Env env;
    const auto id = sineTrackProject (env, 2.0);
    const auto source = firstTrack (env, id);
    env.ok (id, "fx.add", { { "track_id", source }, { "builtin", "compressor" } });
    const auto bus = env.ok (id, "track.create", { { "name", "Bus" } })["track_id"].get<std::string>();
    env.ok (id, "routing.send_create", { { "track_id", source }, { "to_track_id", bus } });
    const auto before = env.ok (id, "track.list")["tracks"].size();

    auto dup = env.run (id, "track.duplicate", { { "track_id", source }, { "name", "Copy" } });
    REQUIRE (dup.ok);
    CHECK (dup.data["name"] == "Copy");
    CHECK (dup.data["items"].size() == 1);
    CHECK (env.ok (id, "track.list")["tracks"].size() == before + 1);

    int compressors = 0, sends = 0;

    for (auto& p : dup.data["plugins"])
    {
        compressors += p["type"] == "compressor" ? 1 : 0;
        sends += p["type"] == "auxsend" ? 1 : 0;
    }

    CHECK (compressors == 1);
    CHECK (sends == 0);
    CHECK (env.ok (id, "routing.list")["sends"].size() == 1);

    // item ids and plugin ids are new
    const auto srcItems = env.ok (id, "item.list", { { "track_id", source } })["items"];
    const auto dupItems = env.ok (id, "item.list", { { "track_id", dup.data["track_id"] } })["items"];
    REQUIRE (dupItems.size() == 1);
    CHECK (dupItems[0]["item_id"] != srcItems[0]["item_id"]);

    env.ok (id, "undo");
    CHECK (env.ok (id, "track.list")["tracks"].size() == before);
    env.ok (id, "redo");
    CHECK (env.ok (id, "track.list")["tracks"].size() == before + 1);

    auto without = env.run (id, "track.duplicate", { { "track_id", source }, { "with_items", false } });
    REQUIRE (without.ok);
    CHECK (without.data["items"].empty());
    CHECK (! env.run (id, "track.duplicate", { { "track_id", "trk_nope" } }).ok);
}

//==============================================================================
ROVOX_TEST (fx, move_reorders_a_chain_and_undo_restores_it)
{
    test::Env env;
    const auto id = sineTrackProject (env, 2.0);
    const auto track = firstTrack (env, id);
    const auto a = env.ok (id, "fx.add", { { "track_id", track }, { "builtin", "compressor" } })["plugin_id"].get<std::string>();
    const auto b = env.ok (id, "fx.add", { { "track_id", track }, { "builtin", "reverb" } })["plugin_id"].get<std::string>();

    auto order = [&]
    {
        std::vector<std::string> ids;

        const auto listed = env.ok (id, "fx.list", { { "target", track } });

        for (auto& f : listed["fx"])
            ids.push_back (f["plugin_id"]);

        return ids;
    };

    const auto start = order();
    REQUIRE (start.size() >= 3);
    CHECK (start[0] == a);
    CHECK (start[1] == b);
    auto moved = env.run (id, "fx.move", { { "plugin_id", b }, { "to_index", 0 } });
    REQUIRE (moved.ok);
    CHECK (moved.data["index"] == 0);
    auto after = order();
    REQUIRE (after.size() >= 2);
    CHECK (after[0] == b);
    CHECK (after[1] == a);
    env.ok (id, "undo");
    after = order();
    REQUIRE (after.size() >= 2);
    CHECK (after[0] == a);
    CHECK (after[1] == b);
    CHECK (! env.run (id, "fx.move", { { "plugin_id", "fx_nope" }, { "to_index", 0 } }).ok);

    // master chain too
    const auto m1 = env.ok (id, "fx.add", { { "track_id", "master" }, { "builtin", "compressor" } })["plugin_id"].get<std::string>();
    const auto m2 = env.ok (id, "fx.add", { { "track_id", "master" }, { "builtin", "equaliser" } })["plugin_id"].get<std::string>();
    env.ok (id, "fx.move", { { "plugin_id", m2 }, { "to_index", 0 } });
    CHECK (env.ok (id, "fx.list", { { "target", "master" } })["fx"][0]["plugin_id"] == m2);
    (void) m1;
}

ROVOX_TEST (fx, presets_are_listed_saved_loaded_and_undone)
{
    test::Env env;
    scanOnce (env);
    const auto uid = pluginUid (env, "effect", "Calf Compressor");

    if (uid.empty())
        return;

    const auto id = sineTrackProject (env, 2.0);
    auto added = env.run (id, "fx.add", { { "track_id", firstTrack (env, id) }, { "plugin", { { "plugin_uid", uid } } } });
    REQUIRE (added.ok);
    const auto plugin = added.data["plugin_id"].get<std::string>();

    auto presets = env.run (id, "fx.presets", { { "plugin_id", plugin } });
    REQUIRE (presets.ok);
    std::cerr << "    " << presets.data["presets"].size() << " factory presets\n";

    // a user preset stores the state; loading it brings the parameter back
    auto params = env.ok (id, "fx.params", { { "plugin_id", plugin } })["fx"]["params"];
    std::string ident;
    double original = 0, lo = 0, hi = 0;

    for (auto& p : params)
        if (p["name"].get<std::string>().find ("Threshold") != std::string::npos)
        {
            ident = p["ident"];
            original = p["value"];
            lo = p["min"];
            hi = p["max"];
        }

    REQUIRE (! ident.empty());
    const double changed = original > (lo + hi) / 2 ? lo + (hi - lo) * 0.2 : lo + (hi - lo) * 0.8;
    env.ok (id, "fx.update", { { "plugin_id", plugin }, { "params", { { ident, changed } } } });
    auto saved = env.run (id, "fx.preset_save", { { "plugin_id", plugin }, { "name", "Loud one" }, { "tags", { "test" } } });
    REQUIRE (saved.ok);

    env.ok (id, "fx.update", { { "plugin_id", plugin }, { "params", { { ident, original } } } });
    auto listed = env.ok (id, "fx.presets", { { "plugin_id", plugin } });
    bool found = false;

    for (auto& p : listed["presets"])
        found = found || (p["name"] == "Loud one" && p["source"] == "user" && p["preset_id"] == saved.data["preset_id"]);

    CHECK (found);

    auto loaded = env.run (id, "fx.preset_load", { { "plugin_id", plugin }, { "preset_id", saved.data["preset_id"] } });
    REQUIRE (loaded.ok);
    auto value = [&]
    {
        const auto all = env.ok (id, "fx.params", { { "plugin_id", plugin } });

        for (auto& p : all["fx"]["params"])
            if (p["ident"] == ident)
                return p["value"].get<double>();

        return -1e9;
    };
    CHECK_NEAR (value(), changed, std::abs (hi - lo) * 0.01);

    env.ok (id, "undo");
    CHECK_NEAR (value(), original, std::abs (hi - lo) * 0.01);
    CHECK (! env.run (id, "fx.preset_load", { { "plugin_id", plugin }, { "preset_id", "pr_nope" } }).ok);
    CHECK (! env.run (id, "fx.presets", { { "plugin_id", "fx_nope" } }).ok);
}

//==============================================================================
ROVOX_TEST (templates, built_in_templates_save_apply_and_undo)
{
    test::Env env;
    auto list = env.run ("", "template.list");
    REQUIRE (list.ok);
    CHECK (list.data["templates"].size() >= 2);
    CHECK (env.run ("", "template.list", { { "tag", "audio" } }).data["templates"].size() == 1);

    const auto id = env.newProject ("tpl");
    auto applied = env.run (id, "template.apply", { { "template_id", "audio-band" } });
    REQUIRE (applied.ok);
    CHECK (applied.data["tracks"].size() == 5);
    auto tracks = env.ok (id, "track.list");
    CHECK (tracks["tracks"].size() == 5);
    CHECK (env.ok (id, "routing.list")["sends"].size() == 2);
    CHECK (env.ok (id, "fx.list", { { "target", "master" } })["fx"].size() >= 1);
    CHECK_NEAR (tracks["tracks"][0]["volume_db"].get<double>(), -3.0, 0.05);
    CHECK (env.ok (id, "history.list")["undo"].size() == 1);                // ONE undo step

    env.ok (id, "undo");
    CHECK (env.ok (id, "track.list")["tracks"].empty());
    CHECK (env.ok (id, "routing.list")["sends"].empty());
    env.ok (id, "redo");
    CHECK (env.ok (id, "track.list")["tracks"].size() == 5);

    // save what we have and apply it elsewhere: the same structure comes back
    auto saved = env.run (id, "template.save", { { "template_id", "my-band" }, { "name", "My band" }, { "tags", { "mine" } } });
    REQUIRE (saved.ok);
    CHECK (saved.data["version"] == 1);
    CHECK (env.run (id, "template.save", { { "template_id", "my-band" }, { "name", "My band" }, { "tags", { "mine" } } }).data["version"] == 2);
    CHECK (env.run ("", "template.get", { { "template_id", "my-band" } }).ok);
    CHECK (env.run ("", "template.list", { { "tag", "mine" } }).data["templates"].size() == 1);

    const auto other = env.newProject ("tpl2");
    REQUIRE (env.run (other, "template.apply", { { "template_id", "my-band" } }).ok);
    std::function<void (json&)> round3 = [&] (json& j)
    {
        if (j.is_number_float())
            j = std::round (j.get<double>() * 1000.0) / 1000.0;
        else if (j.is_structured())
            for (auto& v : j)
                round3 (v);
    };

    auto normalise = [&] (const std::string& project)
    {
        auto doc = env.ok (project, "template.save", { { "template_id", "probe-" + project.substr (project.size() - 6) } })["template"];
        for (auto* k : { "id", "name", "saved_from", "saved_at", "version", "description", "tags" })
            doc.erase (k);

        round3 (doc);
        return doc;
    };

    const auto n1 = normalise (id), n2 = normalise (other);

    if (n1 != n2)
        std::cerr << "    original: " << n1.dump() << "\n    applied:  " << n2.dump() << "\n";

    CHECK (n1 == n2);
    env.run ("", "template.delete", { { "template_id", "my-band" } });
    CHECK (! env.run ("", "template.get", { { "template_id", "my-band" } }).ok);
    CHECK (! env.run ("", "template.delete", { { "template_id", "audio-band" } }).ok);          // built-in
    CHECK (! env.run (id, "template.apply", { { "template_id", "nope" } }).ok);
    CHECK (! env.run (id, "template.apply", { { "template_id", "audio-band" }, { "bindings", { { "tuba", { { "notes", json::array() } } } } } }).ok);
}

ROVOX_TEST (templates, missing_instruments_are_reported_before_anything_is_created)
{
    test::Env env;
    scanOnce (env);
    const auto id = env.newProject ("tpl-missing");
    const auto dir = test::makeTempDir ("tpl");

    // a template that needs an instrument nobody has installed
    json doc { { "id", "needs-ghost" }, { "name", "Ghost" }, { "version", 1 }, { "tracks", json::array ({ { { "role", "lead" }, { "name", "Lead" }, { "instrument", { { "kind", "instrument" }, { "query", "no-such-synth-xyz" } } } } }) },
               { "master", { { "fx", json::array() } } } };
    fs::create_directories (env.pm.host().dataDir() / "templates" / "needs-ghost");
    std::ofstream (env.pm.host().dataDir() / "templates" / "needs-ghost" / "template.json") << doc.dump();

    const auto before = env.project (id).revision();
    auto r = env.run (id, "template.apply", { { "template_id", "needs-ghost" } });
    REQUIRE (! r.ok);
    CHECK (r.error->code == errc::preconditionFailed);
    REQUIRE (r.error->details["missing"].size() == 1);
    CHECK (r.error->details["missing"][0]["where"] == "track lead instrument");
    CHECK (env.project (id).revision() == before);
    CHECK (env.ok (id, "track.list")["tracks"].empty());
    (void) dir;
}

ROVOX_TEST (templates, instrument_template_plays_bound_midi)
{
    test::Env env;
    scanOnce (env);

    if (pluginUid (env, "instrument", "ZynAddSubFX").empty() || pluginUid (env, "instrument", "drum").empty() || pluginUid (env, "instrument", "organ").empty())
        return;

    const auto id = env.newProject ("tpl-band");
    auto applied = env.run (id, "template.apply", { { "template_id", "drums-bass-keys" },
                                                    { "bindings", { { "bass", { { "notes", notes ({ { 36, 0.0 }, { 43, 1.0 } }, 0.9) } } } } } });

    if (! applied.ok)
        std::cerr << "    " << applied.error->toJson().dump() << "\n";

    REQUIRE (applied.ok);
    CHECK (applied.data["items"].size() == 1);
    CHECK (env.ok (id, "track.list")["tracks"].size() == 4);
}

//==============================================================================
ROVOX_TEST (projects, catalog_lists_projects_on_disk_not_only_open_ones)
{
    test::Env env;
    const auto id = env.newProject ("Disk project");
    env.ok (id, "track.create", { { "name", "One" } });
    env.ok (id, "track.create", { { "name", "Two" } });
    REQUIRE (env.project (id).save().ok());
    const auto dir = env.project (id).dir();
    REQUIRE (env.pm.close (id, SaveMode::save).ok());

    CHECK (env.pm.find (id) == nullptr);
    auto list = env.run ("", "project.list");
    bool inOpen = false;

    for (auto& p : list.data["projects"])
        inOpen = inOpen || p["project_id"] == id;

    CHECK (! inOpen);

    auto cat = env.run ("", "project.catalog");
    REQUIRE (cat.ok);
    const json* found = nullptr;

    for (auto& p : cat.data["projects"])
        if (p["project_id"] == id)
            found = &p;

    REQUIRE (found != nullptr);
    CHECK ((*found)["name"] == "Disk project");
    CHECK ((*found)["open"] == false);
    CHECK ((*found)["tracks"] == 2);
    CHECK ((*found)["modified"].get<long long>() > 0);

    auto reopened = env.pm.open (id);
    REQUIRE (reopened.ok());
    bool open = false;

    const auto reopenedCatalog = env.run ("", "project.catalog").data;

    for (auto& p : reopenedCatalog["projects"])
        if (p["project_id"] == id)
            open = p["open"].get<bool>();

    CHECK (open);

    // ephemeral projects stay hidden unless asked for
    const auto temp = env.newProject ("Scratch", true, 3600);
    REQUIRE (env.project (temp).save().ok());
    auto countTemp = [&] (bool includeEphemeral)
    {
        const auto cat2 = env.run ("", "project.catalog", { { "include_ephemeral", includeEphemeral } }).data;
        int n = 0;

        for (auto& p : cat2["projects"])
            n += p["project_id"] == temp ? 1 : 0;

        return n;
    };

    CHECK (countTemp (false) == 1);          // an open ephemeral project is listed (it is open)
    REQUIRE (env.pm.close (temp, SaveMode::save).ok());
    CHECK (countTemp (false) == 0);
    CHECK (countTemp (true) == 1);
}

ROVOX_TEST (projects, delivery_hold_is_a_project_setting_with_undo)
{
    test::Env env;
    const auto id = env.newProject ("hold");
    auto r = env.run (id, "project.update", { { "delivery_hold", true } });
    REQUIRE (r.ok);
    CHECK (r.data["delivery_hold"] == true);
    env.ok (id, "undo");
    CHECK (env.ok (id, "project.update", json::object())["delivery_hold"] == false);
}
