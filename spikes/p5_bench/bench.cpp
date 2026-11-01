// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Spike P5: resource benchmark. Generated projects (tracks x clips x built-in effects [+ synth + reverb returns]) are
//   (a) opened from a saved file (project.open time),
//   (b) rendered offline (speed factor),
//   (c) played in real time through the engine's hosted audio device interface, paced like a sound card
//       (block processing time vs. block period, deadline misses = underruns).
//
//   rovox_spikes bench <work-dir> [--scenario s1|s2|s3|s4|custom] [--tracks N --clips N --fx N] [--seconds D]
//                      [--rt-seconds D] [--no-cgroup] [--ramp] [--json FILE]
// Scenarios (song length --seconds, default 240 s @44.1 kHz, 512-sample blocks):
//   s1: 8 mono tracks x 1 clip x 2 fx (EQ + compressor), master compressor
//   s2: s1 + synth (FourOsc) track with MIDI + 2 reverb tracks
//   s3: 32 tracks x 3 fx (EQ + compressor + reverb)
//   s4: 3 x s1 open; one plays in real time while two render offline concurrently

#include "SpikeUtil.h"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace spike;
using namespace te::literals;

namespace
{
constexpr double kRate = 44100.0;
constexpr int kBlock = 512;

struct Scenario
{
    juce::String name = "custom";
    int tracks = 8, clips = 1, fx = 2;
    bool synth = false;
    int reverbTracks = 0;
    int projects = 1;
    double seconds = 240.0;
};

struct Options
{
    juce::File workDir;
    Scenario sc;
    double rtSeconds = 20.0;
    bool useCgroup = true, ramp = false;
    juce::String jsonFile;
};

long currentRssKb()
{
    const auto s = juce::File ("/proc/self/statm").loadFileAsString();
    juce::StringArray t;
    t.addTokens (s, " ", "");
    return t.size() > 1 ? (long) (t[1].getLargeIntValue() * 4) : 0;
}

int numThreadsNow()
{
    const auto s = juce::File ("/proc/self/status").loadFileAsString();

    for (auto& line : juce::StringArray::fromLines (s))
        if (line.startsWith ("Threads:"))
            return line.fromFirstOccurrenceOf (":", false, false).trim().getIntValue();

    return -1;
}

juce::File makeSource (const Options& o, double seconds)
{
    const auto f = o.workDir.getChildFile ("bench_src_" + juce::String ((int) seconds) + ".wav");

    if (! f.existsAsFile())
    {
        const int n = (int) (seconds * kRate);
        std::vector<float> s ((size_t) n);
        juce::Random rnd (42);

        for (int i = 0; i < n; ++i)
        {
            const double t = i / kRate;
            s[(size_t) i] = (float) (0.2 * std::sin (juce::MathConstants<double>::twoPi * 220 * t) + 0.05 * std::sin (juce::MathConstants<double>::twoPi * 3100 * t)
                                     + 0.03 * (rnd.nextFloat() * 2.0f - 1.0f));
        }

        writeWav (f, { s }, kRate, 24);
    }

    return f;
}

void addBuiltIn (te::Edit& edit, te::PluginList& list, const char* type, int index)
{
    if (auto p = edit.getPluginCache().createNewPlugin (type, {}))
        list.insertPlugin (p, index, nullptr);
}

std::unique_ptr<te::Edit> buildEdit (te::Engine& engine, const Options& o, const Scenario& sc, te::Edit::EditRole role)
{
    auto edit = te::Edit::createSingleTrackEdit (engine, role);
    const int total = sc.tracks + (sc.synth ? 1 : 0) + sc.reverbTracks;
    // ensureNumberOfAudioTracks() re-scans the track list for every insertion (quadratic; minutes for 500+ tracks),
    // so add the tracks directly at the end and keep our own list
    std::vector<te::AudioTrack*> tracks;

    for (auto* t : te::getAudioTracks (*edit))
        tracks.push_back (t);

    while ((int) tracks.size() < total)
    {
        auto* t = edit->insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (*edit), nullptr).get();

        if (t == nullptr)
            break;                              // EditLimits::maxNumTracks reached

        tracks.push_back (t);
    }

    if ((int) tracks.size() < total)
        return edit;

    const auto src = makeSource (o, sc.seconds);
    const double clipLen = sc.seconds / sc.clips;
    static const char* const fxTypes[] = { te::EqualiserPlugin::xmlTypeName, te::CompressorPlugin::xmlTypeName, te::ReverbPlugin::xmlTypeName };

    for (int t = 0; t < sc.tracks; ++t)
    {
        for (int c = 0; c < sc.clips; ++c)
            te::insertWaveClip (*tracks[(size_t) t], {}, src,
                                { .time = { te::TimePosition::fromSeconds (c * clipLen), te::TimeDuration::fromSeconds (clipLen) } },
                                te::DeleteExistingClips::no);

        for (int k = 0; k < sc.fx; ++k)
            addBuiltIn (*edit, tracks[(size_t) t]->pluginList, fxTypes[k % 3], k);
    }

    int next = sc.tracks;

    if (sc.synth)
    {
        auto* tr = tracks[(size_t) next++];
        auto clip = te::insertMIDIClip (*tr, "notes", { 0_tp, te::TimePosition::fromSeconds (sc.seconds) });
        const int beats = (int) (sc.seconds * 2.0);

        for (int b = 0; b < beats; b += 2)
            for (int n : { 48, 55, 60, 64 })
                clip->getSequence().addNote (n, te::BeatPosition::fromBeats (b), te::BeatDuration::fromBeats (1.5), 90, 0, nullptr);

        addBuiltIn (*edit, tr->pluginList, te::FourOscPlugin::xmlTypeName, 0);
    }

    for (int r = 0; r < sc.reverbTracks; ++r)
    {
        auto* tr = tracks[(size_t) next++];
        te::insertWaveClip (*tr, {}, src, { .time = { 0_tp, te::TimeDuration::fromSeconds (sc.seconds) } }, te::DeleteExistingClips::no);
        addBuiltIn (*edit, tr->pluginList, te::ReverbPlugin::xmlTypeName, 0);
    }

    addBuiltIn (*edit, edit->getMasterPluginList(), te::CompressorPlugin::xmlTypeName, 0);
    return edit;
}

struct RtResult
{
    double seconds = 0, p50Us = 0, p99Us = 0, maxUs = 0, periodUs = 0, outRmsDb = -200;
    long blocks = 0, overBudget = 0, lateStarts = 0;
    int threads = 0;
};

/** Plays `edit` through the hosted device, paced at real time, from a non-message thread. Must be called from the message thread.
    The audio thread keeps running blocks all the time (like a sound card); statistics are taken only while `measuring`. */
RtResult playRealtime (te::Engine& engine, te::Edit& edit, double seconds, int* playRetries = nullptr)
{
    RtResult r;
    auto& tc = edit.getTransport();
    tc.ensureContextAllocated();

    auto& hosted = engine.getDeviceManager().getHostedAudioDeviceInterface();
    std::vector<double> times;
    std::atomic<bool> measuring { false }, stop { false };
    double sumSq = 0;
    long nSamples = 0;

    std::thread audio ([&]
    {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds ((long long) (1.0e9 * kBlock / kRate));
        r.periodUs = std::chrono::duration<double, std::micro> (period).count();
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        auto next = clock::now();
        times.reserve ((size_t) (seconds * kRate / kBlock) + 1024);

        while (! stop)
        {
            next += period;
            std::this_thread::sleep_until (next);
            const auto t0 = clock::now();
            const bool lateWake = t0 - next > period;     // the wake-up itself was more than a full block late

            buf.clear();
            midi.clear();
            hosted.processBlock (buf, midi);
            const auto dt = std::chrono::duration<double, std::micro> (clock::now() - t0).count();

            if (! measuring)
                continue;

            if (lateWake)
                ++r.lateStarts;

            times.push_back (dt);

            if (dt > r.periodUs)
                ++r.overBudget;

            if (r.threads == 0 && times.size() > 32)
                r.threads = numThreadsNow();

            for (int c = 0; c < 2; ++c)
            {
                const float* d = buf.getReadPointer (c);

                for (int k = 0; k < kBlock; ++k)
                    sumSq += (double) d[k] * d[k];
            }

            nSamples += 2 * kBlock;
        }
    });

    // Start the transport. TransportControl::timerCallback() stops a transport whose playhead has not started yet, so
    // keep re-issuing play() until it sticks (see the P3 report).
    int retries = 0;
    tc.play (false);

    for (int waited = 0, missed = 0; waited < 150 && missed < 100; ++waited)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        if (tc.isPlaying())
            break;

        if (waited % 3 == 2)
        {
            tc.play (false);
            ++retries;
        }
    }

    if (playRetries != nullptr)
        *playRetries = retries;

    measuring = true;
    const auto t0 = nowMs();

    while (nowMs() - t0 < seconds * 1000.0)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

    measuring = false;
    stop = true;
    audio.join();
    tc.stop (false, false);

    std::sort (times.begin(), times.end());
    r.seconds = seconds;
    r.blocks = (long) times.size();
    r.p50Us = times.empty() ? 0 : times[times.size() / 2];
    r.p99Us = times.empty() ? 0 : times[(size_t) ((double) times.size() * 0.99)];
    r.maxUs = times.empty() ? 0 : times.back();
    r.outRmsDb = 10.0 * std::log10 (std::max (sumSq / std::max (1L, nSamples), 1.0e-20));
    return r;
}

juce::var toVar (const RtResult& r)
{
    return makeObject ({ { "seconds", r.seconds }, { "blocks", (juce::int64) r.blocks }, { "block_period_us", r.periodUs },
                         { "block_time_us_p50", r.p50Us }, { "block_time_us_p99", r.p99Us }, { "block_time_us_max", r.maxUs },
                         { "cpu_load_p50_percent_of_one_core", 100.0 * r.p50Us / r.periodUs },
                         { "over_budget_blocks", (juce::int64) r.overBudget }, { "late_wakeups", (juce::int64) r.lateStarts },
                         { "underrun_per_min", r.seconds > 0 ? (double) (r.overBudget + r.lateStarts) * 60.0 / r.seconds : 0.0 },
                         { "process_threads_during_play", r.threads }, { "output_rms_db", r.outRmsDb } });
}

juce::var runOne (te::Engine& engine, const Options& o, const Scenario& sc)
{
    const auto t0 = nowMs();
    auto edit = buildEdit (engine, o, sc, te::Edit::EditRole::forEditing);
    const auto buildMs = nowMs() - t0;

    // save + reopen = project.open proxy
    const auto file = o.workDir.getChildFile ("bench_" + sc.name + ".tracktionedit");
    te::EditFileOperations (*edit).writeToFile (file, false);
    edit.reset();

    const auto o0 = nowMs();
    auto opened = te::loadEditFromFile (engine, file, te::Edit::EditRole::forEditing);
    const auto openMs = nowMs() - o0;

    juce::var offline, rt;

    {   // offline render of the whole song
        const auto out = o.workDir.getChildFile ("bench_out.wav");
        const auto r0 = nowMs();
        const auto err = renderBlocking (*opened, makeRenderParams (*opened, out, kRate, sc.seconds, kBlock));
        const auto ms = nowMs() - r0;
        offline = makeObject ({ { "error", err }, { "render_ms", ms }, { "song_s", sc.seconds }, { "speed_factor", sc.seconds * 1000.0 / ms } });
        out.deleteFile();
    }

    if (o.rtSeconds > 0)
    {
        std::vector<std::shared_ptr<te::EditRenderer::Handle>> handles;
        std::atomic<int> rendersDone { 0 };
        double bgStart = nowMs();
        std::vector<std::unique_ptr<te::Edit>> bgEdits;
        juce::Array<juce::var> bgResults;
        std::vector<double> bgMs;

        if (sc.projects > 1)
            for (int i = 1; i < sc.projects; ++i)
            {
                bgEdits.push_back (buildEdit (engine, o, sc, te::Edit::EditRole::forRendering));
                auto params = makeRenderParams (*bgEdits.back(), o.workDir.getChildFile ("bg" + juce::String (i) + ".wav"), kRate, sc.seconds, kBlock);
                handles.push_back (te::EditRenderer::render (std::move (params), [&rendersDone, &bgMs, bgStart] (auto) { bgMs.push_back (nowMs() - bgStart); ++rendersDone; }));
            }

        int playRetries = 0;
        const auto rtResult = playRealtime (engine, *opened, o.rtSeconds, &playRetries);
        rt = toVar (rtResult);
        rt.getDynamicObject()->setProperty ("transport_play_retries", playRetries);

        if (sc.projects > 1)
        {
            juce::Array<juce::var> bg;

            for (double v : bgMs)
                bg.add (v);

            rt.getDynamicObject()->setProperty ("background_renders_finished", rendersDone.load());
            rt.getDynamicObject()->setProperty ("background_render_ms", bg);
        }

        handles.clear();
    }

    return makeObject ({ { "scenario", sc.name }, { "tracks", sc.tracks }, { "clips_per_track", sc.clips }, { "fx_per_track", sc.fx },
                         { "synth", sc.synth }, { "reverb_tracks", sc.reverbTracks }, { "projects_open", sc.projects },
                         { "build_ms", buildMs }, { "project_open_ms", openMs }, { "offline", offline }, { "realtime", rt },
                         { "rss_now_mb", currentRssKb() / 1024.0 }, { "rss_peak_mb", peakRssKb() / 1024.0 },
                         { "cgroup_mem_peak_mb", cgroupMemoryPeak() / 1048576.0 } });
}

Scenario scenarioByName (const juce::String& n, double seconds)
{
    Scenario s;
    s.name = n;
    s.seconds = seconds;

    if (n == "s1")      { s.tracks = 8;  s.clips = 1; s.fx = 2; }
    else if (n == "s2") { s.tracks = 8;  s.clips = 1; s.fx = 2; s.synth = true; s.reverbTracks = 2; }
    else if (n == "s3") { s.tracks = 32; s.clips = 1; s.fx = 3; }
    else if (n == "s4") { s.tracks = 8;  s.clips = 1; s.fx = 2; s.projects = 3; }

    return s;
}
}

int runBench (int argc, char** argv)
{
    Options o;

    if (argc < 2)
    {
        std::fprintf (stderr, "usage: rovox_spikes bench <work-dir> [--scenario s1|s2|s3|s4|custom] [--tracks N --clips N --fx N] [--seconds D] [--rt-seconds D] [--no-cgroup] [--ramp] [--json FILE]\n");
        return 2;
    }

    o.workDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    o.workDir.createDirectory();
    juce::String scenario = "s1";
    int tracks = -1, clips = -1, fx = -1;
    double seconds = 240.0;

    for (int i = 2; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        const juce::String v = i + 1 < argc ? juce::String (argv[i + 1]) : juce::String();

        if (a == "--scenario")          { scenario = v; ++i; }
        else if (a == "--tracks")       { tracks = v.getIntValue(); ++i; }
        else if (a == "--clips")        { clips = v.getIntValue(); ++i; }
        else if (a == "--fx")           { fx = v.getIntValue(); ++i; }
        else if (a == "--seconds")      { seconds = v.getDoubleValue(); ++i; }
        else if (a == "--rt-seconds")   { o.rtSeconds = v.getDoubleValue(); ++i; }
        else if (a == "--json")         { o.jsonFile = v; ++i; }
        else if (a == "--no-cgroup")    o.useCgroup = false;
        else if (a == "--ramp")         o.ramp = true;
    }

    o.sc = scenarioByName (scenario, seconds);

    if (tracks > 0) o.sc.tracks = tracks;
    if (clips > 0)  o.sc.clips = clips;
    if (fx >= 0)    o.sc.fx = fx;

    auto behaviour = std::make_unique<SpikeBehaviour>();
    behaviour->autoInit = false;
    behaviour->systemDevices = false;
    behaviour->useCgroup = o.useCgroup;
    behaviour->limits.maxNumTracks = 4096;      // default 400; beyond it Edit::insertTrack() returns null
    te::Engine engine ("rovox_p5_bench", nullptr, std::move (behaviour));

    te::HostedAudioDeviceInterface::Parameters hp;
    hp.sampleRate = kRate;
    hp.blockSize = kBlock;
    hp.inputChannels = 2;
    hp.outputChannels = 2;
    engine.getDeviceManager().getHostedAudioDeviceInterface().initialise (hp);

    const auto env = makeObject ({ { "juce_num_cpus", juce::SystemStats::getNumCpus() }, { "cgroup_cpu_quota", cgroupCpuQuota() },
                                   { "cgroup_memory_limit_mb", cgroupMemoryLimit() / 1048576.0 }, { "cpus_used_by_engine", engine.getEngineBehaviour().getNumberOfCPUsToUseForAudio() },
                                   { "use_cgroup", o.useCgroup } });
    std::printf ("env: %s\n", juce::JSON::toString (env, true).toRawUTF8());

    juce::Array<juce::var> results;

    if (o.ramp)
    {
        for (int t : { 8, 16, 32, 64, 128, 256, 512, 1024, 2048 })
        {
            auto sc = o.sc;
            sc.tracks = t;
            sc.name = "ramp_" + juce::String (t);
            sc.seconds = std::min (seconds, 50.0);   // (a render of exactly 60.000 s hangs, see the P5 report)
            auto r = runOne (engine, o, sc);
            std::printf ("%s\n", juce::JSON::toString (r, true).toRawUTF8());
            results.add (r);

            const double factor = r["offline"]["speed_factor"], missPerMin = r["realtime"]["underrun_per_min"];

            if (factor < 1.0 || missPerMin > 6.0)
            {
                std::printf ("ramp stops at %d tracks (offline %.1fx, %.1f underruns/min)\n", t, factor, missPerMin);
                break;
            }
        }
    }
    else
    {
        auto r = runOne (engine, o, o.sc);
        std::printf ("%s\n", juce::JSON::toString (r, true).toRawUTF8());
        results.add (r);
    }

    auto root = makeObject ({ { "env", env }, { "results", results } });

    if (o.jsonFile.isNotEmpty())
        juce::File::getCurrentWorkingDirectory().getChildFile (o.jsonFile).replaceWithText (juce::JSON::toString (root, false));

    return 0;
}
