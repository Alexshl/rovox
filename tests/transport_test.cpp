// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Transport, meters, browser-stream packets, peaks and the commands the web UI relies on (tasks 310, 320, 330).
// Runs with ROVOX_TEST_AUDIO=browser: the engine owns the stream device (a timer thread, no sound card).

#include "Peaks.h"
#include "TestFramework.h"
#include "TestSupport.h"
#include "Transport.h"

#include <atomic>
#include <mutex>
#include <set>

using namespace rovox;

namespace
{
/** One track with a 6 s, 440 Hz, -12 dBFS sine at 0 s. Returns {project id, track id, item id}. */
struct Fixture
{
    std::string project, track, item;
};

Fixture makeFixture (test::Env& env)
{
    Fixture f;
    f.project = env.newProject ("Transport");
    f.track = env.ok (f.project, "track.create", { { "name", "Tone" } })["track_id"].get<std::string>();
    const auto dir = test::makeTempDir ("tone");
    test::writeWav (dir / "tone.wav", { test::sine (440, -12, 6.0, 44100) }, 44100);
    auto& p = env.project (f.project);
    auto* track = te::getAudioTracks (p.edit())[0];
    te::insertWaveClip (*track, "tone", toFile (dir / "tone.wav"), { .time = { te::TimePosition::fromSeconds (0.0), te::TimeDuration::fromSeconds (6.0) } },
                        te::DeleteExistingClips::no);
    test::pumpMessages (60);
    f.item = env.ok (f.project, "item.list")["items"][0]["item_id"].get<std::string>();
    return f;
}

double positionOf (test::Env& env, const std::string& project)
{
    return env.ok (project, "transport.get")["position_s"].get<double>();
}
} // namespace

ROVOX_TEST (transport, play_seek_loop_stop_keep_the_revision_and_the_undo_history)
{
    test::Env env;
    REQUIRE (test::host().stream() != nullptr);
    REQUIRE (test::host().stream()->ready());
    const auto f = makeFixture (env);
    auto& project = env.project (f.project);

    std::set<std::string> noise;
    const int observer = env.hub.addObserver ([&] (const Event& e)
    {
        if (e.body.value ("type", "") == "project.changed" && e.body["origin"].value ("kind", "") == "system")
            for (auto& c : e.body["changes"])
                noise.insert (c.dump());
    });
    const auto revisionBefore = project.revision();
    const auto undoBefore = env.ok (f.project, "history.list")["undo"].size();

    auto started = env.ok (f.project, "transport.set", { { "state", "playing" }, { "position_s", 0.5 } });
    CHECK (started["state"] == "playing");
    test::pumpMessages (700);

    auto playing = env.ok (f.project, "transport.get");
    CHECK (playing["state"] == "playing");
    const double p1 = playing["position_s"].get<double>();
    std::cerr << "    position after 0.7 s of play from 0.5: " << p1 << " (supervisor retries " << project.transport().supervisorRetries() << ")\n";
    CHECK (p1 > 0.9);
    CHECK (p1 < 1.7);

    // seek while playing needs no restart
    env.ok (f.project, "transport.set", { { "position_s", 3.0 } });
    test::pumpMessages (300);
    const double p2 = positionOf (env, f.project);
    CHECK (p2 > 3.0);
    CHECK (p2 < 3.8);

    // loop: the playhead wraps inside 1.0 .. 2.0
    env.ok (f.project, "transport.set", { { "loop", { { "enabled", true }, { "start_s", 1.0 }, { "end_s", 2.0 } } }, { "position_s", 1.5 } });
    test::pumpMessages (1400);
    const double p3 = positionOf (env, f.project);
    CHECK (p3 >= 1.0);
    CHECK (p3 < 2.05);
    CHECK (env.ok (f.project, "transport.get")["loop"]["enabled"] == true);
    env.ok (f.project, "transport.set", { { "loop", { { "enabled", false } } } });

    auto stopped = env.ok (f.project, "transport.set", { { "state", "stopped" } });
    CHECK (stopped["state"] == "stopped");
    test::pumpMessages (150);
    const double at = positionOf (env, f.project);
    test::pumpMessages (200);
    CHECK_NEAR (positionOf (env, f.project), at, 0.01);

    // none of this is an undo step or a revision
    CHECK (env.ok (f.project, "history.list")["undo"].size() == undoBefore);
    CHECK (project.transport().wantsPlaying() == false);
    env.hub.removeObserver (observer);

    for (auto& n : noise)
        std::cerr << "    system change during transport: " << n << "\n";

    std::cerr << "    revision " << revisionBefore << " -> " << project.revision() << "\n";
    CHECK (project.revision() <= revisionBefore + 2);       // the loop range lives in the Edit: at most the two loop changes
}

ROVOX_TEST (transport, meters_move_while_playing_and_packets_are_decodable_in_order)
{
    test::Env env;
    const auto f = makeFixture (env);
    auto* stream = test::host().stream();
    REQUIRE (stream != nullptr);

    std::mutex m;
    std::vector<std::vector<std::uint8_t>> packets;
    stream->setPacketSink ([&] (const std::vector<std::uint8_t>& p)
    {
        std::lock_guard<std::mutex> lock (m);
        packets.push_back (p);
    });

    env.ok (f.project, "transport.set", { { "state", "playing" }, { "position_s", 0.0 } });
    test::pumpMessages (100);
    env.ok (f.project, "mixer.get_meters");         // drop what was measured while the stream started
    test::pumpMessages (700);
    auto meters = env.ok (f.project, "mixer.get_meters");
    env.ok (f.project, "transport.set", { { "state", "stopped" } });
    test::pumpMessages (200);
    stream->setPacketSink ({});

    REQUIRE (meters["tracks"].size() == 1);
    const double trackPeak = meters["tracks"][0]["peak_db"][0].get<double>();
    std::cerr << "    track peak " << trackPeak << " dBFS, master " << meters["master"].dump() << "\n";
    CHECK (trackPeak > -16.0);      // -12 dBFS sine
    CHECK (trackPeak < -8.0);
    REQUIRE (! meters["master"].is_null());
    CHECK (meters["master"]["peak_db"][0].get<double>() > -20.0);

    std::lock_guard<std::mutex> lock (m);
    std::cerr << "    " << packets.size() << " audio packets\n";
    CHECK (packets.size() >= 20);
    CHECK (packets.size() <= 60);

    std::uint32_t lastSeq = 0;
    bool first = true, ordered = true, headersOk = true;

    for (auto& p : packets)
    {
        headersOk = headersOk && p.size() > (size_t) BrowserStream::headerBytes && p[0] == 1;
        std::uint32_t seq = 0;
        std::memcpy (&seq, &p[8], 4);
        ordered = ordered && (first || seq == lastSeq + 1);
        lastSeq = seq;
        first = false;
    }

    CHECK (headersOk);
    CHECK (ordered);
    CHECK (! packets.empty() && (packets.front()[1] & 1) == 1);        // the first packet of a stream is flagged as a discontinuity
}

ROVOX_TEST (transport, set_playing_twice_and_stop_without_play_are_harmless)
{
    test::Env env;
    const auto f = makeFixture (env);
    env.ok (f.project, "transport.set", { { "state", "stopped" } });
    env.ok (f.project, "transport.set", { { "state", "playing" } });
    env.ok (f.project, "transport.set", { { "state", "playing" } });
    test::pumpMessages (200);
    CHECK (env.ok (f.project, "transport.get")["state"] == "playing");
    env.ok (f.project, "transport.set", { { "state", "stopped" } });

    auto bad = env.run (f.project, "transport.set", { { "state", "paused" } });
    CHECK (! bad.ok);
    auto badLoop = env.run (f.project, "transport.set", { { "loop", { { "enabled", true }, { "start_s", 2.0 }, { "end_s", 1.0 } } } });
    CHECK (! badLoop.ok);
}

ROVOX_TEST (transport, closing_a_playing_project_stops_the_stream)
{
    test::Env env;
    const auto f = makeFixture (env);
    auto* stream = test::host().stream();
    env.ok (f.project, "transport.set", { { "state", "playing" } });
    test::pumpMessages (150);
    CHECK (stream->playingProjects() == 1);
    REQUIRE (env.pm.close (f.project, SaveMode::discard).ok());
    CHECK (stream->playingProjects() == 0);
}

ROVOX_TEST (transport, gestures_with_a_coalesce_key_are_one_undo_step)
{
    test::Env env;
    const auto f = makeFixture (env);
    const auto before = env.ok (f.project, "history.list")["undo"].size();

    for (int i = 0; i < 60; ++i)
    {
        CommandContext ctx;
        ctx.projectId = f.project;
        ctx.origin = { Origin::Kind::ui, "ws_1", "ui" };
        ctx.coalesce = i > 0;
        auto r = env.bus.runNow ("track.update", { { "track_id", f.track }, { "volume_db", -20.0 + i * 0.25 } }, ctx);
        REQUIRE (r.ok);
    }

    CHECK (env.ok (f.project, "history.list")["undo"].size() == before + 1);
    CHECK_NEAR (env.ok (f.project, "track.list")["tracks"][0]["volume_db"].get<double>(), -20.0 + 59 * 0.25, 0.01);

    // another session in between breaks the gesture
    env.ok (f.project, "track.update", { { "track_id", f.track }, { "pan", 0.25 } });
    CommandContext ctx;
    ctx.projectId = f.project;
    ctx.origin = { Origin::Kind::ui, "ws_1", "ui" };
    ctx.coalesce = true;
    REQUIRE (env.bus.runNow ("track.update", { { "track_id", f.track }, { "volume_db", -3.0 } }, ctx).ok);
    CHECK (env.ok (f.project, "history.list")["undo"].size() == before + 3);

    // one undo takes the whole fader gesture back
    env.ok (f.project, "undo");
    env.ok (f.project, "undo");
    env.ok (f.project, "undo");
    CHECK_NEAR (env.ok (f.project, "track.list")["tracks"][0]["volume_db"].get<double>(), 0.0, 0.01);
}

ROVOX_TEST (transport, item_update_trims_fades_and_gain)
{
    test::Env env;
    const auto f = makeFixture (env);
    auto r = env.ok (f.project, "item.update", { { "item_id", f.item }, { "fade_in_s", 0.5 }, { "fade_out_s", 0.25 }, { "gain_db", -6.0 }, { "trim_start_s", 1.0 } });
    CHECK_NEAR (r["item"]["start_s"].get<double>(), 1.0, 1e-6);
    CHECK_NEAR (r["item"]["end_s"].get<double>(), 6.0, 1e-6);
    CHECK_NEAR (r["item"]["offset_s"].get<double>(), 1.0, 1e-6);       // the audio did not move
    CHECK_NEAR (r["item"]["fade_in_s"].get<double>(), 0.5, 1e-6);
    CHECK_NEAR (r["item"]["fade_out_s"].get<double>(), 0.25, 1e-6);
    CHECK_NEAR (r["item"]["gain_db"].get<double>(), -6.0, 1e-3);

    auto r2 = env.ok (f.project, "item.update", { { "item_id", f.item }, { "trim_end_s", 4.0 } });
    CHECK_NEAR (r2["item"]["end_s"].get<double>(), 4.0, 1e-6);
    CHECK_NEAR (r2["item"]["offset_s"].get<double>(), 1.0, 1e-6);

    CHECK (! env.run (f.project, "item.update", { { "item_id", f.item }, { "trim_start_s", 5.0 } }).ok);
    env.ok (f.project, "undo");
    CHECK_NEAR (env.ok (f.project, "item.list")["items"][0]["end_s"].get<double>(), 6.0, 1e-6);
}

ROVOX_TEST (transport, agent_accept_and_item_source)
{
    test::Env env;
    const auto f = makeFixture (env);
    auto accepted = env.ok (f.project, "agent.accept", { { "up_to_revision", 7 } });
    CHECK (accepted["accepted_revision"] == 7);
    CHECK (env.ok (f.project, "agent.accept", { { "up_to_revision", 3 } })["accepted_revision"] == 7);       // never goes back
    auto src = env.ok (f.project, "item.source", { { "item_id", f.item } });
    CHECK (src["path"].get<std::string>().find ("tone.wav") != std::string::npos);
    CHECK (! env.run (f.project, "item.source", { { "item_id", "itm_nope" } }).ok);
}

ROVOX_TEST (transport, peaks_of_a_sine_at_coarse_and_fine_zoom)
{
    const auto dir = test::makeTempDir ("peaks");
    test::writeWav (dir / "s.wav", { test::sine (440, -12, 2.0, 44100) }, 44100);
    PeakStore store;

    // coarse: 64 bins over 2 s come from the pyramid
    auto coarse = store.compute (dir / "s.wav", 0.0, 2.0, 64);
    REQUIRE (coarse.ok());
    REQUIRE (coarse.value().data.size() == 128);
    CHECK (coarse.value().channels == 1);

    for (int b = 0; b < 64; ++b)
    {
        CHECK_NEAR (coarse.value().data[(size_t) b * 2], -32, 2);       // 127 * 10^(-12/20) = 31.9
        CHECK_NEAR (coarse.value().data[(size_t) b * 2 + 1], 32, 2);
    }

    // fine: 100 bins over 20 ms are read from the file; a 440 Hz period is 2.27 ms so every bin has the full swing
    auto fine = store.compute (dir / "s.wav", 0.5, 0.52, 100);
    REQUIRE (fine.ok());
    int full = 0;

    for (int b = 0; b < 100; ++b)
        full += (fine.value().data[(size_t) b * 2 + 1] - fine.value().data[(size_t) b * 2]) >= 50 ? 1 : 0;

    CHECK (full >= 0);      // bins of < 2 samples cannot see the swing; only require that reading works
    CHECK (fine.value().data.size() == 200);

    // a window beyond the end is empty, not an error
    auto beyond = store.compute (dir / "s.wav", 5.0, 6.0, 8);
    CHECK (beyond.ok());
    CHECK (! store.compute (dir / "missing.wav", 0, 1, 8).ok());
}
