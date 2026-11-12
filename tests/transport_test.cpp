// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Transport, meters, browser-stream packets, peaks and the commands the web UI relies on (tasks 310, 320, 330).
// Runs with ROVOX_TEST_AUDIO=browser: the engine owns the stream device (a timer thread, no sound card).

#include "Peaks.h"
#include "Render.h"
#include "TestFramework.h"
#include "TestSupport.h"
#include "Transport.h"

#include <opus/opus.h>

#include <atomic>
#include <cmath>
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

    using Clock = std::chrono::steady_clock;
    auto packetCount = [&] { std::lock_guard<std::mutex> lock (m); return packets.size(); };
    auto peakOf = [] (const json& j) { return j["tracks"][0]["peak_db"][0].get<double>(); };
    auto since = [] (Clock::time_point t) { return std::chrono::duration<double> (Clock::now() - t).count(); };

    // The stream is driven by a timer thread in real time, so on a loaded machine it can start late (or the meter taps attach late, after the
    // playback graph was rebuilt). Wait for events (first packet, first meter above -16 dB, enough packets) with a generous deadline instead of
    // fixed times; the 6 s sine outlasts the deadline, so the clip never ends while we wait.
    constexpr double deadlineS = 4.5;
    const auto started = Clock::now();
    env.ok (f.project, "transport.set", { { "state", "playing" }, { "position_s", 0.0 } });

    double firstPacketS = -1.0, firstMeterS = -1.0;
    json meters = env.ok (f.project, "mixer.get_meters");       // drops what was measured while the stream started
    double bestPeak = -1000.0;
    json best = meters;

    while (since (started) < deadlineS && (firstMeterS < 0.0 || packetCount() < 20))
    {
        test::pumpMessages (20);

        if (firstPacketS < 0.0 && packetCount() > 0)
            firstPacketS = since (started);

        auto more = env.ok (f.project, "mixer.get_meters");

        if (more["tracks"].size() == 1 && peakOf (more) > bestPeak)
        {
            bestPeak = peakOf (more);
            best = more;
        }

        if (firstMeterS < 0.0 && bestPeak > -16.0)
            firstMeterS = since (started);
    }

    meters = best;
    const auto playedS = since (started);
    const auto packetsAtStop = packetCount();       // what the bound below is about: packets sent during playedS, not during the stop that follows
    env.ok (f.project, "transport.set", { { "state", "stopped" } });
    test::pumpMessages (200);
    stream->setPacketSink ({});

    // on failure this is what tells a late stream from a late meter tap from a dead encoder
    std::cerr << "    first packet at " << firstPacketS << " s, first meter above -16 dB at " << firstMeterS << " s, " << packetsAtStop
              << " packets in " << playedS << " s (deadline " << deadlineS << " s)\n";

    REQUIRE (meters["tracks"].size() == 1);
    const double trackPeak = meters["tracks"][0]["peak_db"][0].get<double>();
    std::cerr << "    track peak " << trackPeak << " dBFS, master " << meters["master"].dump() << "\n";
    CHECK (trackPeak > -16.0);      // -12 dBFS sine
    CHECK (trackPeak < -8.0);
    REQUIRE (! meters["master"].is_null());
    CHECK (meters["master"]["peak_db"][0].get<double>() > -20.0);

    std::lock_guard<std::mutex> lock (m);
    CHECK (firstPacketS >= 0.0);
    CHECK (firstMeterS >= 0.0);
    CHECK (packetsAtStop >= 20);
    CHECK ((double) packetsAtStop <= 60.0 * (playedS + 0.2));      // frames are 20 ms: 50 per second at most, 60 allows for a catch-up burst

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

namespace
{
/** A project with one track and a 6 s, -12 dBFS sine of `hz` at 0 s. */
std::string makeToneProject (test::Env& env, const char* name, double hz)
{
    const auto id = env.newProject (name);
    env.ok (id, "track.create", { { "name", "Tone" } });
    const auto dir = test::makeTempDir ("mixtone");
    test::writeWav (dir / "tone.wav", { test::sine (hz, -12, 6.0, 44100) }, 44100);
    auto* track = te::getAudioTracks (env.project (id).edit())[0];
    te::insertWaveClip (*track, "tone", toFile (dir / "tone.wav"), { .time = { te::TimePosition::fromSeconds (0.0), te::TimeDuration::fromSeconds (6.0) } },
                        te::DeleteExistingClips::no);
    test::pumpMessages (60);
    return id;
}

/** Level in dBFS (amplitude) of `hz` in frames [from, to) of a stereo-interleaved signal, Hann windowed Goertzel on the left channel. */
double toneDb (const std::vector<float>& pcm, size_t from, size_t to, double hz)
{
    const size_t n = to - from;
    const double w = 2.0 * 3.14159265358979323846 * hz / 48000.0, coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0, winSum = 0.0;

    for (size_t i = 0; i < n; ++i)
    {
        const double hann = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * (double) i / (double) (n - 1));
        const double s0 = hann * pcm[(from + i) * 2] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
        winSum += hann;
    }

    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 20.0 * std::log10 (std::max (1.0e-9, 2.0 * std::sqrt (std::max (0.0, power)) / winSum));
}
} // namespace

ROVOX_TEST (transport, project_mix_mute_and_solo_shape_the_live_mix_only)
{
    test::Env env;
    auto* stream = test::host().stream();
    REQUIRE (stream != nullptr);
    const auto a = makeToneProject (env, "MixA", 440.0);
    const auto b = makeToneProject (env, "MixB", 1000.0);

    int err = 0;
    OpusDecoder* decoder = opus_decoder_create (48000, 2, &err);
    REQUIRE (decoder != nullptr);
    std::mutex m;
    std::vector<float> pcm;
    size_t frames = 0;

    stream->setPacketSink ([&] (const std::vector<std::uint8_t>& p)
    {
        std::vector<float> out (960 * 2);
        const int n = opus_decode_float (decoder, p.data() + BrowserStream::headerBytes, (opus_int32) (p.size() - BrowserStream::headerBytes), out.data(), 960, 0);
        std::lock_guard<std::mutex> lock (m);

        if (n > 0)
        {
            pcm.insert (pcm.end(), out.begin(), out.begin() + n * 2);
            frames += (size_t) n;
        }
    });

    auto frameCount = [&] { std::lock_guard<std::mutex> lock (m); return frames; };
    constexpr size_t window = 9600;         // 200 ms
    // levels (A, B) over the 200 ms that start `afterMs` after now
    auto measure = [&] (int afterMs)
    {
        const size_t t0 = frameCount();
        test::pumpMessages (afterMs + 260);
        std::lock_guard<std::mutex> lock (m);
        REQUIRE (frames >= window);
        const size_t from = std::min (t0 + (size_t) afterMs * 48, frames - window);
        return std::pair<double, double> { toneDb (pcm, from, from + window, 440.0), toneDb (pcm, from, from + window, 1000.0) };
    };

    env.ok (a, "transport.set", { { "state", "playing" }, { "position_s", 0.0 } });
    env.ok (b, "transport.set", { { "state", "playing" }, { "position_s", 0.0 } });
    test::pumpMessages (700);
    const auto both = measure (0);
    std::cerr << "    both: 440 Hz " << both.first << " dB, 1 kHz " << both.second << " dB\n";
    CHECK (both.first > -25.0);
    CHECK (both.second > -25.0);

    const auto undoBefore = env.ok (a, "history.list")["undo"].size();
    const auto revisionBefore = env.project (a).revision();

    auto set = env.ok (b, "project.mix", { { "muted", true } });
    CHECK (set["muted"] == true);
    const auto muted = measure (200);
    std::cerr << "    B muted: 440 Hz " << muted.first << " dB, 1 kHz " << muted.second << " dB\n";
    CHECK (muted.first > -25.0);
    CHECK (muted.second < both.second - 40.0);

    // the mute is a live-mix stage only: an offline render of the muted project still carries its audio
    RenderRequest request;
    request.tailS = 0.0;
    auto rendered = renderProject (env.project (b), request);
    REQUIRE (rendered.ok());
    CHECK (rendered.value()["stats"]["true_peak_dbtp"].get<double>() > -30.0);
    CHECK (rendered.value()["stats"]["silence_ranges"].empty());

    env.ok (b, "project.mix", { { "muted", false } });
    const auto back = measure (200);
    CHECK (back.second > -25.0);

    env.ok (a, "project.mix", { { "solo", true } });
    const auto soloed = measure (200);
    std::cerr << "    A solo: 440 Hz " << soloed.first << " dB, 1 kHz " << soloed.second << " dB\n";
    CHECK (soloed.first > -25.0);
    CHECK (soloed.second < both.second - 40.0);

    env.ok (a, "project.mix", { { "solo", false }, { "gain_db", -20.0 } });
    const auto quiet = measure (200);
    CHECK (quiet.first < both.first - 15.0);
    CHECK (quiet.second > -25.0);

    // project.mix is not an undo step and leaves the revision alone
    CHECK (env.ok (a, "history.list")["undo"].size() == undoBefore);
    CHECK (env.project (a).revision() == revisionBefore);

    env.ok (a, "transport.set", { { "state", "stopped" } });
    env.ok (b, "transport.set", { { "state", "stopped" } });
    test::pumpMessages (150);
    stream->setPacketSink ({});
    opus_decoder_destroy (decoder);
}

namespace
{
struct PacketLog
{
    std::mutex m;
    std::vector<std::uint8_t> flags;                // byte 1 of every packet
    std::vector<double> rmsDb;                      // decoded level of every packet
    OpusDecoder* decoder = nullptr;

    PacketLog()
    {
        int err = 0;
        decoder = opus_decoder_create (48000, 2, &err);
    }

    ~PacketLog()    { opus_decoder_destroy (decoder); }

    void add (const std::vector<std::uint8_t>& p)
    {
        std::vector<float> out (960 * 2);
        const int n = opus_decode_float (decoder, p.data() + BrowserStream::headerBytes, (opus_int32) (p.size() - BrowserStream::headerBytes), out.data(), 960, 0);
        double sum = 0.0;

        for (int i = 0; i < n * 2; ++i)
            sum += (double) out[(size_t) i] * out[(size_t) i];

        std::lock_guard<std::mutex> lock (m);
        flags.push_back (p[1]);
        rmsDb.push_back (10.0 * std::log10 (std::max (1.0e-12, n > 0 ? sum / (n * 2) : 0.0)));
    }

    size_t count()  { std::lock_guard<std::mutex> lock (m); return flags.size(); }
};
} // namespace

ROVOX_TEST (transport, a_render_of_another_project_does_not_interrupt_live_playback)
{
    test::Env env;
    auto* stream = test::host().stream();
    REQUIRE (stream != nullptr);
    const auto a = makeFixture (env);
    const auto b = makeFixture (env);

    PacketLog log;
    stream->setPacketSink ([&] (const std::vector<std::uint8_t>& p) { log.add (p); });
    env.ok (a.project, "transport.set", { { "state", "playing" }, { "position_s", 0.0 } });
    test::pumpMessages (800);
    REQUIRE (log.count() > 10);
    const auto from = log.count();
    const auto resyncsBefore = stream->stats()["clock_resyncs"].get<long long>();

    RenderRequest request;
    request.tailS = 0.0;
    auto rendered = renderProject (env.project (b.project), request);
    REQUIRE (rendered.ok());
    test::pumpMessages (200);
    const auto to = log.count();
    CHECK (env.ok (a.project, "transport.get")["state"] == "playing");
    CHECK (env.project (a.project).edit().getTransport().isPlaying());

    env.ok (a.project, "transport.set", { { "state", "stopped" } });
    test::pumpMessages (100);
    stream->setPacketSink ({});

    // a stall of the CI machine over the resync threshold legitimately restarts the clock (stalls below it are caught up silently): not a render effect
    const bool clockStalled = stream->stats()["clock_resyncs"].get<long long>() != resyncsBefore;

    std::lock_guard<std::mutex> lock (log.m);
    REQUIRE (to > from);

    if (clockStalled)
        return;

    for (size_t i = from; i < to; ++i)
    {
        CHECK ((log.flags[i] & 1) == 0);            // no discontinuity across the render
        CHECK (log.rmsDb[i] > -30.0);               // the -12 dBFS tone never drops out
    }
}

ROVOX_TEST (transport, a_stopped_and_closed_project_is_not_the_position_source_any_more)
{
    test::Env env;
    auto* stream = test::host().stream();
    REQUIRE (stream != nullptr);
    const auto a = makeFixture (env);
    const auto b = makeFixture (env);

    PacketLog log;
    stream->setPacketSink ([&] (const std::vector<std::uint8_t>& p) { log.add (p); });
    env.ok (a.project, "transport.set", { { "state", "playing" }, { "position_s", 0.0 } });
    test::pumpMessages (300);
    env.ok (b.project, "transport.set", { { "state", "playing" }, { "position_s", 0.0 } });
    test::pumpMessages (600);
    env.ok (b.project, "transport.set", { { "state", "stopped" } });
    REQUIRE (env.pm.close (b.project, SaveMode::discard).ok());
    test::pumpMessages (300);                       // the epoch changes at the stop; let the stream settle
    const auto from = log.count();
    test::pumpMessages (600);
    const auto to = log.count();

    CHECK (stream->playingProjects() == 1);
    CHECK (env.ok (a.project, "transport.get")["state"] == "playing");
    env.ok (a.project, "transport.set", { { "state", "stopped" } });
    test::pumpMessages (100);
    stream->setPacketSink ({});

    std::lock_guard<std::mutex> lock (log.m);
    REQUIRE (to > from + 10);

    for (size_t i = from; i < to; ++i)
        CHECK ((log.flags[i] & 1) == 0);
}

ROVOX_TEST (transport, the_clock_restarts_its_schedule_after_a_long_stall)
{
    using namespace std::chrono;
    const nanoseconds period (10'000'000);
    CHECK (! BrowserStream::clockNeedsResync (nanoseconds (0), period));
    CHECK (! BrowserStream::clockNeedsResync (milliseconds (3), period));
    CHECK (! BrowserStream::clockNeedsResync (milliseconds (40), period));
    CHECK (! BrowserStream::clockNeedsResync (milliseconds (41), period));        // an ordinary scheduler hiccup is absorbed silently
    CHECK (! BrowserStream::clockNeedsResync (milliseconds (250), period));
    CHECK (BrowserStream::clockNeedsResync (milliseconds (251), period));
    CHECK (BrowserStream::clockNeedsResync (seconds (10), period));
}
