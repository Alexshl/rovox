// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Resource benchmark (task 170, ported from spike P5). Scenarios (song length --seconds, 44.1 kHz, 512-sample blocks):
//   s1: 8 mono tracks x 1 clip x 2 fx (Rovox EQ + Comp), master Rovox Comp + Limit
//   s2: s1 + synth (FourOsc) track with MIDI + 2 reverb tracks
//   s3: 32 tracks x 3 fx (EQ + compressor + reverb)
//   s4: 3 x s1 open; one plays in real time while two render offline concurrently
//   s5: 16 tracks of 44.1 kHz media in a 48 kHz project, real-time playback; clip resampling quality from ROVOX_BENCH_RESAMPLING
//       (sincBest | sincMedium | sincFast | lagrange; default sincMedium, the live-playback quality)

#include "Bench.h"

#include "EngineHost.h"
#include "StockPlugins.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <sys/resource.h>
#include <unistd.h>

using namespace tracktion::literals;

namespace rovox::cli
{

namespace
{
constexpr double kRate = 44100.0;
constexpr int kBlock = 512;

// Device / project rate; differs from kRate (the rate of the generated media) only in s5.
double gDeviceRate = kRate;

struct Scenario
{
    std::string name;
    int tracks = 8, clips = 1, fx = 2;
    bool synth = false;
    int reverbTracks = 0;
    int projects = 1;
    double seconds = 20.0;
    std::optional<te::ResamplingQuality> quality;
};

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli> (steady_clock::now().time_since_epoch()).count();
}

long peakRssKb()
{
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
    return (long) ru.ru_maxrss;
}

juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();

    for (auto& p : props)
        o->setProperty (p.first, p.second);

    return juce::var (o);
}

int numThreadsNow()
{
    for (auto& line : juce::StringArray::fromLines (juce::File ("/proc/self/status").loadFileAsString()))
        if (line.startsWith ("Threads:"))
            return line.fromFirstOccurrenceOf (":", false, false).trim().getIntValue();

    return -1;
}

void writeWav (const juce::File& file, const std::vector<float>& samples)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());

    if (stream == nullptr)
        return;

    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), kRate, 1u, 24, {}, 0));

    if (writer == nullptr)
        return;

    stream.release();
    juce::AudioBuffer<float> buffer (1, (int) samples.size());
    buffer.copyFrom (0, 0, samples.data(), (int) samples.size());
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

juce::File makeSource (const juce::File& dir, double seconds)
{
    const auto f = dir.getChildFile ("bench_src_" + juce::String ((int) seconds) + ".wav");

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

        writeWav (f, s);
    }

    return f;
}

juce::String stockType (const char* shortId)
{
    const auto* info = findStockPlugin (shortId);
    return info != nullptr ? juce::String (info->xmlType) : juce::String();
}

void addPlugin (te::Edit& edit, te::PluginList& list, const juce::String& type, int index)
{
    if (auto p = edit.getPluginCache().createNewPlugin (type, {}))
        list.insertPlugin (p, index, nullptr);
}

std::unique_ptr<te::Edit> buildEdit (te::Engine& engine, const juce::File& dir, const Scenario& sc, te::Edit::EditRole role)
{
    auto edit = te::Edit::createSingleTrackEdit (engine, role);
    const int total = sc.tracks + (sc.synth ? 1 : 0) + sc.reverbTracks;
    std::vector<te::AudioTrack*> tracks;

    for (auto* t : te::getAudioTracks (*edit))
        tracks.push_back (t);

    while ((int) tracks.size() < total)
    {
        auto* t = edit->insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (*edit), nullptr).get();

        if (t == nullptr)
            break;

        tracks.push_back (t);
    }

    if ((int) tracks.size() < total)
        return edit;

    const auto src = makeSource (dir, sc.seconds);
    const double clipLen = sc.seconds / sc.clips;
    const juce::String fxTypes[] = { stockType ("eq"), stockType ("comp"), stockType ("verb") };

    for (int t = 0; t < sc.tracks; ++t)
    {
        for (int c = 0; c < sc.clips; ++c)
        {
            auto clip = te::insertWaveClip (*tracks[(size_t) t], {}, src,
                                            { .time = { te::TimePosition::fromSeconds (c * clipLen), te::TimeDuration::fromSeconds (clipLen) } },
                                            te::DeleteExistingClips::no);

            if (clip != nullptr && sc.quality.has_value())
                clip->state.setProperty (te::IDs::resamplingQuality, juce::VariantConverter<te::ResamplingQuality>::toVar (*sc.quality), nullptr);
        }

        for (int k = 0; k < sc.fx; ++k)
            addPlugin (*edit, tracks[(size_t) t]->pluginList, fxTypes[k % 3], k);
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

        addPlugin (*edit, tr->pluginList, te::FourOscPlugin::xmlTypeName, 0);
    }

    for (int r = 0; r < sc.reverbTracks; ++r)
    {
        auto* tr = tracks[(size_t) next++];
        te::insertWaveClip (*tr, {}, src, { .time = { 0_tp, te::TimeDuration::fromSeconds (sc.seconds) } }, te::DeleteExistingClips::no);
        addPlugin (*edit, tr->pluginList, stockType ("verb"), 0);
    }

    addPlugin (*edit, edit->getMasterPluginList(), stockType ("comp"), 0);
    addPlugin (*edit, edit->getMasterPluginList(), stockType ("limit"), 1);
    return edit;
}

te::Renderer::Parameters makeRenderParams (te::Edit& edit, const juce::File& dest, double seconds)
{
    te::Renderer::Parameters p (edit);
    p.destFile = dest;
    p.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
    p.bitDepth = 24;
    p.blockSizeForAudio = kBlock;
    p.sampleRateForAudio = gDeviceRate;
    p.time = { te::TimePosition::fromSeconds (0.0), te::TimeDuration::fromSeconds (seconds) };
    p.canRenderInMono = false;
    return p;
}

/** Offline render on the calling thread. Returns an error string, empty on success. */
juce::String renderBlocking (te::Edit& edit, te::Renderer::Parameters params)
{
    const te::Edit::ScopedRenderStatus srs (edit, true);
    te::TransportControl::stopAllTransports (edit.engine, false, true);

    params.destFile.deleteFile();
    te::Renderer::RenderTask task ("bench", params, nullptr, nullptr);

    if (task.errorMessage.isNotEmpty())
        return task.errorMessage;

    while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
    {}

    if (task.errorMessage.isNotEmpty())
        return task.errorMessage;

    return params.destFile.existsAsFile() ? juce::String() : juce::String ("no output file");
}

struct RtResult
{
    double seconds = 0, p50Us = 0, p99Us = 0, maxUs = 0, periodUs = 0, outRmsDb = -200;
    long blocks = 0, overBudget = 0, lateStarts = 0;
    int threads = 0;
};

/** Plays `edit` through the hosted device, paced at real time, from a non-message thread. Call on the message thread. */
RtResult playRealtime (te::Engine& engine, te::Edit& edit, double seconds, int* playRetries)
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
        const auto period = std::chrono::nanoseconds ((long long) (1.0e9 * kBlock / gDeviceRate));
        r.periodUs = std::chrono::duration<double, std::micro> (period).count();
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        auto next = clock::now();
        times.reserve ((size_t) (seconds * gDeviceRate / kBlock) + 1024);

        while (! stop)
        {
            next += period;
            std::this_thread::sleep_until (next);
            const auto t0 = clock::now();
            const bool lateWake = t0 - next > period;

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

    // TransportControl::timerCallback() stops a transport whose playhead has not started yet: keep re-issuing play() until it sticks.
    int retries = 0;
    tc.play (false);

    for (int waited = 0; waited < 150; ++waited)
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
    r.outRmsDb = 10.0 * std::log10 (std::max (sumSq / (double) std::max (1L, nSamples), 1.0e-20));
    return r;
}

juce::var toVar (const RtResult& r)
{
    return makeObject ({ { "seconds", r.seconds }, { "blocks", (juce::int64) r.blocks }, { "block_period_us", r.periodUs },
                         { "block_time_us_p50", r.p50Us }, { "block_time_us_p99", r.p99Us }, { "block_time_us_max", r.maxUs },
                         { "cpu_load_p50_percent_of_one_core", r.periodUs > 0 ? 100.0 * r.p50Us / r.periodUs : 0.0 },
                         { "over_budget_blocks", (juce::int64) r.overBudget }, { "late_wakeups", (juce::int64) r.lateStarts },
                         { "underrun_per_min", r.seconds > 0 ? (double) (r.overBudget + r.lateStarts) * 60.0 / r.seconds : 0.0 },
                         { "process_threads_during_play", r.threads }, { "output_rms_db", r.outRmsDb } });
}

juce::var runScenario (te::Engine& engine, const juce::File& dir, const Scenario& sc)
{
    const auto t0 = nowMs();
    auto edit = buildEdit (engine, dir, sc, te::Edit::EditRole::forEditing);
    const auto buildMs = nowMs() - t0;

    // save + reopen = project.open proxy
    const auto file = dir.getChildFile ("bench_" + juce::String (sc.name) + ".tracktionedit");
    te::EditFileOperations (*edit).writeToFile (file, false);
    edit.reset();

    const auto o0 = nowMs();
    auto opened = te::loadEditFromFile (engine, file, te::Edit::EditRole::forEditing);
    const auto openMs = nowMs() - o0;

    if (opened == nullptr)
        return makeObject ({ { "scenario", juce::String (sc.name) }, { "error", "the saved benchmark project did not open" } });

    juce::var offline, rt;

    {
        const auto out = dir.getChildFile ("bench_out.wav");
        const auto r0 = nowMs();
        const auto err = renderBlocking (*opened, makeRenderParams (*opened, out, sc.seconds));
        const auto ms = nowMs() - r0;
        offline = makeObject ({ { "error", err }, { "render_ms", ms }, { "song_s", sc.seconds }, { "speed_factor", sc.seconds * 1000.0 / std::max (ms, 0.001) } });
        out.deleteFile();
    }

    {
        std::vector<std::shared_ptr<te::EditRenderer::Handle>> handles;
        std::atomic<int> rendersDone { 0 };
        const double bgStart = nowMs();
        std::vector<std::unique_ptr<te::Edit>> bgEdits;
        std::vector<double> bgMs;
        std::mutex bgMutex;

        for (int i = 1; i < sc.projects; ++i)
        {
            bgEdits.push_back (buildEdit (engine, dir, sc, te::Edit::EditRole::forRendering));
            auto params = makeRenderParams (*bgEdits.back(), dir.getChildFile ("bg" + juce::String (i) + ".wav"), sc.seconds);
            handles.push_back (te::EditRenderer::render (std::move (params), [&rendersDone, &bgMs, &bgMutex, bgStart] (auto)
            {
                std::lock_guard<std::mutex> lock (bgMutex);
                bgMs.push_back (nowMs() - bgStart);
                ++rendersDone;
            }));
        }

        int playRetries = 0;
        rt = toVar (playRealtime (engine, *opened, sc.seconds, &playRetries));
        rt.getDynamicObject()->setProperty ("transport_play_retries", playRetries);

        if (sc.projects > 1)
        {
            juce::Array<juce::var> bg;

            {
                std::lock_guard<std::mutex> lock (bgMutex);

                for (double v : bgMs)
                    bg.add (v);
            }

            rt.getDynamicObject()->setProperty ("background_renders_finished", rendersDone.load());
            rt.getDynamicObject()->setProperty ("background_render_ms", bg);
        }

        handles.clear();
    }

    return makeObject ({ { "scenario", juce::String (sc.name) }, { "tracks", sc.tracks }, { "clips_per_track", sc.clips }, { "fx_per_track", sc.fx },
                         { "synth", sc.synth }, { "reverb_tracks", sc.reverbTracks }, { "device_rate_hz", gDeviceRate }, { "media_rate_hz", kRate },
                         { "resampling_quality", sc.quality.has_value() ? juce::VariantConverter<te::ResamplingQuality>::toVar (*sc.quality) : juce::var ("default") }, { "projects_open", sc.projects },
                         { "build_ms", buildMs }, { "project_open_ms", openMs }, { "offline", offline }, { "realtime", rt },
                         { "rss_peak_mb", peakRssKb() / 1024.0 } });
}
} // namespace

int runBench (const Options& o)
{
    Scenario sc;
    sc.name = o.benchScenario;
    sc.seconds = o.benchSeconds;

    if (sc.seconds < 1.0 || sc.seconds > 3600.0)
    {
        std::cerr << "error: --seconds must be between 1 and 3600\n";
        return 1;
    }

    if (sc.name == "s1")        { sc.tracks = 8;  sc.fx = 2; }
    else if (sc.name == "s2")   { sc.tracks = 8;  sc.fx = 2; sc.synth = true; sc.reverbTracks = 2; }
    else if (sc.name == "s3")   { sc.tracks = 32; sc.fx = 3; }
    else if (sc.name == "s4")   { sc.tracks = 8;  sc.fx = 2; sc.projects = 3; }
    else if (sc.name == "s5")
    {
        sc.tracks = 16; sc.fx = 0;
        gDeviceRate = 48000.0;
        sc.quality = te::ResamplingQuality::sincMedium;

        if (const char* q = std::getenv ("ROVOX_BENCH_RESAMPLING"); q != nullptr && *q != 0)
        {
            const std::string name (q);

            if (name == "sincBest")         sc.quality = te::ResamplingQuality::sincBest;
            else if (name == "sincMedium")  sc.quality = te::ResamplingQuality::sincMedium;
            else if (name == "sincFast")    sc.quality = te::ResamplingQuality::sincFast;
            else if (name == "lagrange")    sc.quality = te::ResamplingQuality::lagrange;
            else
            {
                std::cerr << "error: ROVOX_BENCH_RESAMPLING must be sincBest | sincMedium | sincFast | lagrange\n";
                return 1;
            }
        }
    }
    else
    {
        std::cerr << "error: unknown scenario '" << sc.name << "' (s1 | s2 | s3 | s4 | s5)\n";
        return 1;
    }

    EngineConfig config;
    config.dataDir = resolveDataDir (o.dataDir);
    EngineHost host (config);

    te::HostedAudioDeviceInterface::Parameters hp;
    hp.sampleRate = gDeviceRate;
    hp.blockSize = kBlock;
    hp.inputChannels = 2;
    hp.outputChannels = 2;
    host.engine().getDeviceManager().getHostedAudioDeviceInterface().initialise (hp);

    // a directory of this run only: concurrent runs and anything else in the data dir stay untouched
    const auto dir = juce::File (host.dataDir().string()).getChildFile ("bench-" + juce::String ((int) ::getpid()) + "-"
                                                                       + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));

    if (! dir.createDirectory().wasOk())
    {
        std::cerr << "error: cannot create " << dir.getFullPathName() << "\n";
        return 1;
    }

    const auto env = makeObject ({ { "juce_num_cpus", juce::SystemStats::getNumCpus() }, { "cpus_used_by_engine", host.cpuBudget().cpus } });
    auto result = runScenario (host.engine(), dir, sc);
    dir.deleteRecursively();

    const auto report = makeObject ({ { "env", env }, { "results", juce::Array<juce::var> { result } } });
    std::cout << juce::JSON::toString (report, ! o.json).toStdString() << std::endl;
    return result.hasProperty ("error") ? 1 : 0;
}

} // namespace rovox::cli
