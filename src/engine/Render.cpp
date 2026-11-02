// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Render.h"

#include "AudioSink.h"
#include "JsfxPlugin.h"
#include "Limiter.h"
#include "Loudness.h"
#include "Sha256.h"
#include "Version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rovox
{

json Finding::toJson() const
{
    json j { { "check", check }, { "severity", severity }, { "message", message } };

    if (! trackId.empty())  j["track_id"] = trackId;
    if (! name.empty())     j["name"] = name;
    if (! details.is_null()) j["details"] = details;
    return j;
}

bool PreflightReport::passed() const
{
    return std::none_of (findings.begin(), findings.end(), [] (const Finding& f) { return f.severity == "fail"; });
}

json PreflightReport::toJson() const
{
    json warnings = json::array(), failures = json::array();

    for (auto& f : findings)
        (f.severity == "fail" ? failures : warnings).push_back (f.toJson());

    return { { "passed", passed() }, { "warnings", warnings }, { "failures", failures } };
}

//==============================================================================
namespace
{
std::string fmt (double v, int decimals = 1)
{
    char b[32];
    std::snprintf (b, sizeof (b), "%.*f", decimals, v);
    return b;
}

std::string trackIdOf (const te::Track* t)
{
    return t != nullptr ? Project::idOf (t->state) : std::string();
}

std::string sanitizeName (std::string n)
{
    for (auto& c : n)
        if (c == '/' || c == '\\' || c == 0)
            c = '_';

    while (! n.empty() && n.front() == '.')
        n.erase (n.begin());

    return n.empty() ? "mix" : n;
}

/** Severity of a check under the request: "fail", "warn" or "" (not requested, dropped). */
std::string severityFor (const RenderRequest& req, const std::string& check)
{
    if (req.failOn.count (check))   return "fail";
    if (req.warnOn.count (check))   return "warn";
    return {};
}

std::optional<double> thresholdOf (const std::set<std::string>& set, const std::string& prefix)
{
    for (auto& e : set)
        if (e.rfind (prefix, 0) == 0)
            try { return std::stod (e.substr (prefix.size())); } catch (...) {}

    return std::nullopt;
}

void addFinding (PreflightReport& r, const RenderRequest& req, const std::string& check, const std::string& message, const te::Track* track = nullptr, json details = nullptr)
{
    const auto sev = severityFor (req, check);

    if (sev.empty())
        return;

    Finding f;
    f.check = check;
    f.severity = sev;
    f.trackId = trackIdOf (track);
    f.name = track != nullptr ? toStd (track->getName()) : std::string();
    f.message = message;
    f.details = std::move (details);
    r.findings.push_back (std::move (f));
}

bool isReadableAudio (const juce::File& f)
{
    if (! f.existsAsFile())
        return false;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    return r != nullptr && r->lengthInSamples > 0;
}

te::Plugin* findInstrument (te::Track& t)
{
    for (auto* p : t.pluginList.getPlugins())
        if (p->isSynth() && p->isEnabled())
            return p;

    return nullptr;
}

/** Plays the plugin instance by hand (a note, then silence) and measures every output channel of every bus,
    not only 0/1 (spike P2: a multi-out drum kit routed the kick to another pair). */
constexpr double silentPeak = 3.16e-4;      // -70 dBFS

struct PluginProbe
{
    double peak = 0;
    std::vector<double> channelPeaks;
};

PluginProbe probePluginInstance (juce::AudioPluginInstance& inst, double sampleRate, int note)
{
    constexpr int blockSize = 512;
    inst.setRateAndBufferSizeDetails (sampleRate, blockSize);
    inst.prepareToPlay (sampleRate, blockSize);
    const int channels = std::max ({ 2, inst.getTotalNumInputChannels(), inst.getTotalNumOutputChannels() });
    juce::AudioBuffer<float> buf (channels, blockSize);
    PluginProbe out;
    out.channelPeaks.assign ((size_t) inst.getTotalNumOutputChannels(), 0.0);

    // Instruments that load samples in a worker thread (avldrums) are silent until that finishes, in wall-clock time, however fast the blocks are
    // processed. So the note is repeated, the blocks are paced like real time until sound is heard, and the probe ends once it has heard a note.
    constexpr int maxBlocks = 300;                      // about 3.5 s at 44.1 kHz and 512 samples
    constexpr int phaseLength = 40;

    for (int block = 0; block < maxBlocks; ++block)
    {
        juce::MidiBuffer midi;
        const int phase = block % phaseLength;

        if (phase == 0)     midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
        if (phase == 25)    midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);

        buf.clear();
        inst.processBlock (buf, midi);

        for (int c = 0; c < inst.getTotalNumOutputChannels(); ++c)
        {
            const double p = buf.getMagnitude (c, 0, blockSize);
            out.channelPeaks[(size_t) c] = std::max (out.channelPeaks[(size_t) c], p);
            out.peak = std::max (out.peak, p);
        }

        if (out.peak > silentPeak)
        {
            if (phase >= 26)
                break;
        }
        else
        {
            juce::Thread::sleep (8);
        }
    }

    inst.releaseResources();
    return out;
}

} // namespace

//==============================================================================
void primePlugins (te::Edit& edit, double sampleRate, int blocks)
{
    for (auto p : te::getAllPlugins (edit, false))
        if (auto* ext = dynamic_cast<te::ExternalPlugin*> (p))
            if (auto* inst = ext->getAudioPluginInstance())
            {
                constexpr int blockSize = 512;
                inst->prepareToPlay (sampleRate, blockSize);
                juce::AudioBuffer<float> buf (std::max ({ 2, inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels() }), blockSize);
                juce::MidiBuffer midi;

                for (int i = 0; i < blocks; ++i)
                {
                    buf.clear();
                    inst->processBlock (buf, midi);
                }

                inst->releaseResources();
            }
}

Result<te::TimeRange> resolveRange (te::Edit& edit, const RenderRange& r)
{
    auto barToTime = [&edit] (int bar)
    {
        const auto numerator = edit.tempoSequence.getTimeSigAt (te::TimePosition()).numerator.get();
        return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (double ((bar - 1) * numerator)));
    };

    te::TimePosition start, end = te::TimePosition::fromSeconds (edit.getLength().inSeconds());

    if (r.startBar)     start = barToTime (*r.startBar);
    if (r.endBar)       end = barToTime (*r.endBar);
    if (r.startS)       start = te::TimePosition::fromSeconds (*r.startS);
    if (r.endS)         end = te::TimePosition::fromSeconds (*r.endS);

    if (end <= start)
        return makeError (errc::preconditionFailed, "nothing to render: the range is empty (project length "
                                                       + std::to_string (edit.getLength().inSeconds()) + " s)");

    return te::TimeRange (start, end);
}

PreflightReport runPreflight (te::Edit& edit, const RenderRequest& req, te::TimeRange range, double sampleRate, const fs::path& tempDir)
{
    PreflightReport report;
    int probeCounter = 0;
    std::error_code mkdirError;
    fs::create_directories (tempDir, mkdirError);

    for (auto* ct : te::getClipTracks (edit))
    {
        if (ct->isMuted (false))
            continue;

        bool anyClip = false;

        for (auto* clip : ct->getClips())
        {
            if (clip->isMuted() || ! clip->getPosition().time.overlaps (range))
                continue;

            anyClip = true;

            // 1. media must be readable
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
            {
                const auto file = audio->getOriginalFile();

                if (! isReadableAudio (file))
                    addFinding (report, req, "offline_media", "media file is missing or unreadable: " + toStd (file.getFullPathName()), ct,
                                json { { "item_id", Project::idOf (clip->state) }, { "file", toStd (file.getFullPathName()) } });
            }
        }

        // 3. empty audio tracks (instrument tracks are handled below)
        if (auto* at = dynamic_cast<te::AudioTrack*> (ct); at != nullptr && ! anyClip && findInstrument (*at) == nullptr)
            addFinding (report, req, "empty_track", "track has no clips in the rendered range", at);
    }

    // 2. plugins that failed to load are silently bypassed by the engine
    for (auto p : te::getAllPlugins (edit, false))
        if (auto* ext = dynamic_cast<te::ExternalPlugin*> (p); ext != nullptr && ext->isEnabled())
            if (ext->getAudioPluginInstance() == nullptr || ext->getLoadError().isNotEmpty())
            {
                const auto* owner = ext->getOwnerTrack();
                const auto reason = ext->getLoadError().isNotEmpty() ? toStd (ext->getLoadError()) : std::string ("no plugin instance");
                addFinding (report, req, "plugin_not_loaded", "plugin '" + toStd (ext->getName()) + "' did not load (" + reason + "); the render would silently bypass it",
                            owner, json { { "plugin_id", Project::idOf (ext->state) }, { "plugin", toStd (ext->getName()) }, { "reason", reason } });
            }

    // 2b. JSFX plugins whose file is missing under ROVOX_JSFX_PATH or does not compile pass the audio through unchanged
    for (auto p : te::getAllPlugins (edit, false))
        if (auto extras = jsfxPluginExtras (*p); extras.is_object() && p->isEnabled() && ! extras["loaded"].get<bool>())
        {
            const auto reason = extras["load_error"].get<std::string>();
            addFinding (report, req, "plugin_not_loaded", "JSFX '" + extras["jsfx_file"].get<std::string>() + "' did not load (" + reason + "); the render would pass the audio through",
                        p->getOwnerTrack(), json { { "plugin_id", Project::idOf (p->state) }, { "plugin", extras["jsfx_file"] }, { "reason", reason } });
        }

    // 4. instruments: every MIDI track with notes in range must produce sound
    for (auto* at : te::getAudioTracks (edit))
    {
        if (at->isMuted (false))
            continue;

        te::MidiClip* firstClip = nullptr;
        const te::MidiNote* firstNote = nullptr;
        te::TimePosition firstTime;

        for (auto* clip : at->getClips())
            if (auto* midi = dynamic_cast<te::MidiClip*> (clip); midi != nullptr && ! midi->isMuted() && midi->getPosition().time.overlaps (range))
                for (auto* n : midi->getSequence().getNotes())
                {
                    const auto t = midi->getTimeOfContentBeat (n->getStartBeat());

                    if (t >= range.getStart() && t < range.getEnd() && (firstNote == nullptr || t < firstTime))
                    {
                        firstClip = midi;
                        firstNote = n;
                        firstTime = t;
                    }
                }

        if (firstNote == nullptr)
            continue;

        json trackInfo { { "track_id", trackIdOf (at) }, { "name", toStd (at->getName()) }, { "kind", "instrument" } };
        auto* instrument = findInstrument (*at);

        if (instrument == nullptr)
        {
            addFinding (report, req, "silent_track", "track has MIDI notes in range but no instrument plugin", at);
            trackInfo["audible"] = false;
        }
        else if (auto* ext = dynamic_cast<te::ExternalPlugin*> (instrument); ext != nullptr && ext->getAudioPluginInstance() == nullptr)
        {
            trackInfo["audible"] = false;     // already reported as plugin_not_loaded
        }
        else if (auto* ext2 = dynamic_cast<te::ExternalPlugin*> (instrument))
        {
            const auto p = probePluginInstance (*ext2->getAudioPluginInstance(), sampleRate, firstNote->getNoteNumber());
            trackInfo["audible"] = p.peak > silentPeak;
            trackInfo["peak_dbfs"] = linearToDb (p.peak);

            json chans = json::array();

            for (auto cp : p.channelPeaks)
                chans.push_back (linearToDb (cp));

            trackInfo["output_channel_peaks_dbfs"] = chans;

            if (p.peak <= silentPeak)
                addFinding (report, req, "silent_track", "instrument '" + toStd (ext2->getName()) + "' produced no audio on any output bus for note "
                                                             + std::to_string (firstNote->getNoteNumber()), at, json { { "output_channel_peaks_dbfs", chans } });
        }
        else
        {
            // built-in instrument: solo render of a short window around the first note
            const auto from = firstTime;
            const auto to = std::min (range.getEnd(), firstTime + te::TimeDuration::fromSeconds (1.0));
            juce::Array<te::Track*> solo;
            solo.add (at);
            const auto probeFile = tempDir / (".probe-" + std::to_string (++probeCounter) + ".wav");
            double peak = 0.0;

            if (auto r = renderRawWav (edit, probeFile, { from, to }, 0.0, sampleRate, te::toBitSet (solo), false, 30.0, false); r.ok())
            {
                if (auto reader = openFloatReader (probeFile); reader.ok())
                    for (std::int64_t pos = 0; pos < reader.value()->totalFrames(); pos += 16384)
                    {
                        const int n = (int) std::min<std::int64_t> (16384, reader.value()->totalFrames() - pos);

                        if (const auto* planar = reader.value()->read (pos, n))
                            for (unsigned c = 0; c < reader.value()->channels(); ++c)
                                for (int i = 0; i < n; ++i)
                                    peak = std::max (peak, (double) std::abs (planar[c][i]));
                    }
            }

            std::error_code ec;
            fs::remove (probeFile, ec);
            trackInfo["audible"] = peak > silentPeak;
            trackInfo["peak_dbfs"] = linearToDb (peak);

            if (peak <= silentPeak)
                addFinding (report, req, "silent_track", "instrument produced no audio in range", at);
        }

        report.tracks.push_back (trackInfo);
        (void) firstClip;
    }

    return report;
}

//==============================================================================
bool needsSixtySecondWorkaround (double lengthS)
{
    return std::abs (lengthS - 60.0) < 1.0e-6;
}

Result<void> renderRawWav (te::Edit& edit, const fs::path& dest, te::TimeRange range, double tailS, double sampleRate,
                           const juce::BigInteger& tracks, bool useMasterPlugins, double stallTimeoutS, bool applyWorkaround,
                           const RenderHooks* hooks, double progressFrom, double progressTo)
{
    // Spike P5: a render of exactly 60.000 s never finishes (main thread parked in NodeRenderContext::renderNextBlock, 0-byte file).
    // Rendering one sample longer avoids it; the caller trims the extra sample (see renderEdit).
    if (applyWorkaround && needsSixtySecondWorkaround (range.getLength().inSeconds()))
        range = range.withEnd (range.getEnd() + te::TimeDuration::fromSeconds (1.0 / sampleRate));

    const te::Edit::ScopedRenderStatus srs (edit, true);
    te::TransportControl::stopAllTransports (edit.engine, false, true);

    auto outFile = toFile (dest);
    outFile.deleteFile();

    te::Renderer::Parameters p (edit);
    p.destFile = outFile;
    p.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
    p.bitDepth = 32;                                // float WAV: no clipping inside the render, checks run on the float data
    p.blockSizeForAudio = 512;
    p.sampleRateForAudio = sampleRate;
    p.time = range;
    p.endAllowance = te::TimeDuration::fromSeconds (tailS);
    p.canRenderInMono = false;
    p.usePlugins = true;
    p.useMasterPlugins = useMasterPlugins;
    p.checkNodesForAudio = false;                   // silent output is reported by our own checks instead of an engine error
    p.tracksToDo = tracks;

    std::atomic<float> progress { 0.0f };
    te::Renderer::RenderTask task ("rovox render", p, &progress, nullptr);

    if (task.errorMessage.isNotEmpty())
        return makeError (errc::internal, "render could not start: " + toStd (task.errorMessage));

    // Watchdog: cancel a render whose progress does not move (a hang inside the engine must not block the server forever)
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    std::atomic<bool> stalled { false }, cancelled { false };

    std::thread watchdog ([&]
    {
        auto lastProgress = progress.load();
        auto lastChange = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock (m);

        while (! cv.wait_for (lock, std::chrono::milliseconds (100), [&] { return finished; }))
        {
            if (hooks != nullptr && hooks->onProgress)
                hooks->onProgress (progressFrom + (progressTo - progressFrom) * (double) progress.load(), "rendering");

            if (hooks != nullptr && hooks->shouldCancel && hooks->shouldCancel())
            {
                cancelled = true;
                task.signalJobShouldExit();
                return;
            }

            if (progress.load() != lastProgress)
            {
                lastProgress = progress.load();
                lastChange = std::chrono::steady_clock::now();
            }
            else if (std::chrono::duration<double> (std::chrono::steady_clock::now() - lastChange).count() > stallTimeoutS)
            {
                stalled = true;
                task.signalJobShouldExit();
                return;
            }
        }
    });

    while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
    {}

    {
        std::lock_guard<std::mutex> lock (m);
        finished = true;
    }

    cv.notify_all();
    watchdog.join();

    if (cancelled)
    {
        outFile.deleteFile();
        return makeError (errc::cancelled, "the render was cancelled");
    }

    if (stalled)
    {
        outFile.deleteFile();
        return makeError (errc::busy, "render made no progress for " + std::to_string ((int) stallTimeoutS) + " s and was cancelled");
    }

    if (task.errorMessage.isNotEmpty())
    {
        outFile.deleteFile();
        return makeError (errc::internal, "render failed: " + toStd (task.errorMessage));
    }

    if (! outFile.existsAsFile())
        return makeError (errc::internal, "render produced no file");

    return {};
}

//==============================================================================
namespace
{
/** Deletes its files unless released: no half-written render may stay in renders/. */
struct TempFiles
{
    std::vector<fs::path> files;
    ~TempFiles() { std::error_code ec; for (auto& f : files) fs::remove (f, ec); }
    void keep (const fs::path& f) { files.erase (std::remove (files.begin(), files.end(), f), files.end()); }
};

struct OneOutput
{
    std::string fileName, trackName;        // e.g. "mix.wav"
    juce::BigInteger tracks;
    bool masterPlugins = true;
    int index = 0;
    double progressFrom = 0.0, progressTo = 1.0;
};

struct OutputResult
{
    json file;
    json statsBefore, statsAfter;
    json loudness;
    std::vector<Finding> postFindings;
    bool failed = false;
};

std::string extensionOf (const std::string& container)
{
    return container == "flac" ? "flac" : (container == "mp3" ? "mp3" : "wav");
}

Result<OutputResult> produce (te::Edit& edit, const RenderContext& ctx, const RenderRequest& req, const OneOutput& out, te::TimeRange range, double sr, TempFiles& temps)
{
    OutputResult res;
    const auto rawPath = ctx.rendersDir / ("." + out.fileName + ".raw.wav");
    const auto partPath = ctx.rendersDir / ("." + out.fileName + ".part");
    temps.files.push_back (rawPath);
    temps.files.push_back (partPath);

    auto cancelRequested = [&] { return req.hooks.shouldCancel && req.hooks.shouldCancel(); };
    auto progressAt = [&] (double f, const char* stage)
    {
        if (req.hooks.onProgress)
            req.hooks.onProgress (out.progressFrom + (out.progressTo - out.progressFrom) * f, stage);
    };

    if (auto r = renderRawWav (edit, rawPath, range, req.tailS, sr, out.tracks, out.masterPlugins, req.stallTimeoutS, true, &req.hooks,
                               out.progressFrom, out.progressFrom + (out.progressTo - out.progressFrom) * 0.7); ! r.ok())
        return r.error();

    // pass 1: measure the float render
    auto reader = openFloatReader (rawPath);

    if (! reader.ok())
        return reader.error();

    const bool trimmed = needsSixtySecondWorkaround (range.getLength().inSeconds()) && req.tailS <= 0.0;
    std::int64_t frameLimit = reader.value()->totalFrames();

    if (trimmed)
        frameLimit = std::min (frameLimit, (std::int64_t) std::llround (range.getLength().inSeconds() * sr));       // drop the sample added by the workaround

    StatsAccumulator before ((int) reader.value()->channels(), sr);

    for (std::int64_t pos = 0; pos < frameLimit;)
    {
        if (cancelRequested())
            return makeError (errc::cancelled, "the render was cancelled");

        const int n = (int) std::min<std::int64_t> (65536, frameLimit - pos);
        const auto* planar = reader.value()->read (pos, n);

        if (planar == nullptr)
            return makeError (errc::io, "cannot read the rendered file back");

        before.add (planar, n);
        pos += n;
        progressAt (0.7 + 0.15 * (double) pos / (double) std::max<std::int64_t> (1, frameLimit), "measuring");
    }

    const auto statsBefore = before.finish();

    SinkSpec spec;
    spec.container = req.format.container;
    spec.bitDepth = req.format.bitDepth;
    spec.bitrateKbps = req.format.bitrateKbps;
    spec.sampleRate = sr;
    spec.channels = req.format.channels;

    // gain for loudness normalisation
    double gainDb = 0.0;
    bool limitedByPeak = false;

    // master limiter: the whole mix is held in memory (channel layout of the output), gain + limiter iterate until the target is met
    const bool useLimiter = req.loudness.mode != "check" && req.loudness.limiter.value_or (req.loudness.mode == "normalize");
    std::vector<std::vector<float>> limited;
    LimiterResult limiterResult;
    int limiterPasses = 0;

    if (useLimiter)
    {
        const int srcCh = (int) reader.value()->channels();
        std::vector<std::vector<float>> mixedAll ((size_t) spec.channels);

        for (auto& c : mixedAll)
            c.reserve ((size_t) frameLimit);

        for (std::int64_t pos = 0; pos < frameLimit;)
        {
            if (cancelRequested())
                return makeError (errc::cancelled, "the render was cancelled");

            const int n = (int) std::min<std::int64_t> (65536, frameLimit - pos);
            const auto* planar = reader.value()->read (pos, n);

            if (planar == nullptr)
                return makeError (errc::io, "cannot read the rendered file back");

            for (int c = 0; c < spec.channels; ++c)
                for (int i = 0; i < n; ++i)
                    mixedAll[(size_t) c].push_back (spec.channels == 1 && srcCh > 1 ? 0.5f * (planar[0][i] + planar[1][i]) : planar[std::min (c, srcCh - 1)][i]);

            pos += n;
        }

        const bool normalizing = req.loudness.mode == "normalize" && statsBefore.integratedLufs > -70.0;
        const double target = req.loudness.targetLufs;
        const double ceiling = req.loudness.truePeakCeilingDb;
        double bestErr = 1.0e9, lastGain = 0.0, prevGain = 0.0, prevLufs = 0.0;
        bool havePrev = false;

        if (normalizing)
            gainDb = target - analyse (mixedAll, sr).integratedLufs;

        for (int pass = 0; pass < (normalizing ? 4 : 1); ++pass)
        {
            if (cancelRequested())
                return makeError (errc::cancelled, "the render was cancelled");

            auto trial = mixedAll;
            LimiterSettings ls;
            ls.ceilingDbtp = ceiling;
            LimiterResult lr;
            AudioStats st;

            for (int attempt = 0; attempt < 4; ++attempt)
            {
                if (attempt > 0)
                    trial = mixedAll;

                lr = limitTruePeak (trial, sr, gainDb, ls);
                st = analyse (trial, sr);

                if (st.truePeakDbtp <= ceiling + 0.01)
                    break;

                ls.ceilingDbtp -= st.truePeakDbtp - ceiling + 0.02;     // the detector missed an inter-sample peak: tighten and redo
            }

            ++limiterPasses;
            const double err = normalizing ? std::abs (st.integratedLufs - target) : 0.0;

            if (err < bestErr)
            {
                bestErr = err;
                limited = std::move (trial);
                limiterResult = lr;
                lastGain = gainDb;
            }

            progressAt (0.7 + 0.15 * (double) (pass + 1) / 4.0, "limiting");

            if (! normalizing || err <= 0.1 || st.integratedLufs < -70.0)
                break;

            // next gain: secant on the (limited) loudness response, which is flatter than 1 dB/dB once the limiter works
            double slope = 1.0;

            if (havePrev && std::abs (gainDb - prevGain) > 1.0e-6)
                slope = std::clamp ((st.integratedLufs - prevLufs) / (gainDb - prevGain), 0.15, 1.0);

            havePrev = true;
            prevGain = gainDb;
            prevLufs = st.integratedLufs;
            gainDb += std::clamp ((target - st.integratedLufs) / slope, -20.0, 20.0);
        }

        gainDb = lastGain;
        limitedByPeak = normalizing && bestErr > 0.1 && limiterResult.maxGainReductionDb > 0.05;
    }
    else if (req.loudness.mode == "normalize" && statsBefore.integratedLufs > -70.0)
    {
        gainDb = req.loudness.targetLufs - statsBefore.integratedLufs;
        const double predictedPeak = statsBefore.truePeakDbtp + gainDb;

        if (predictedPeak > req.loudness.truePeakCeilingDb)
        {
            gainDb -= predictedPeak - req.loudness.truePeakCeilingDb;
            limitedByPeak = true;
        }
    }

    const float gain = useLimiter ? 1.0f : (float) std::pow (10.0, gainDb / 20.0);

    // pass 2: gain, channel layout, dither, encode, measure what is written
    auto sink = createSink (spec, partPath);

    if (! sink.ok())
        return sink.error();

    StatsAccumulator after (spec.channels, sr);
    std::vector<std::vector<float>> mixed ((size_t) spec.channels);
    const int srcChannels = (int) reader.value()->channels();

    for (std::int64_t pos = 0; pos < frameLimit;)
    {
        if (cancelRequested())
            return makeError (errc::cancelled, "the render was cancelled");

        const int n = (int) std::min<std::int64_t> (65536, frameLimit - pos);
        const auto* planar = reader.value()->read (pos, n);

        if (planar == nullptr)
            return makeError (errc::io, "cannot read the rendered file back");

        for (int c = 0; c < spec.channels && ! useLimiter; ++c)
        {
            auto& dst = mixed[(size_t) c];
            dst.resize ((size_t) n);

            for (int i = 0; i < n; ++i)
            {
                // mono target from a stereo render: average; stereo target from a mono render: duplicate
                const float v = spec.channels == 1 && srcChannels > 1 ? 0.5f * (planar[0][i] + planar[1][i])
                                                                      : planar[std::min (c, srcChannels - 1)][i];
                dst[(size_t) i] = v * gain;
            }
        }

        std::vector<const float*> ptrs;

        for (int c = 0; c < spec.channels; ++c)
            ptrs.push_back (useLimiter ? limited[(size_t) c].data() + pos : mixed[(size_t) c].data());

        after.add (ptrs.data(), n);

        if (req.hooks.audioTap)
            req.hooks.audioTap (out.index, ptrs.data(), spec.channels, n);

        if (! sink.value()->write (ptrs.data(), n))
            return makeError (errc::io, "cannot write " + partPath.string());

        pos += n;
        progressAt (0.85 + 0.15 * (double) pos / (double) std::max<std::int64_t> (1, frameLimit), "encoding");
    }

    if (! sink.value()->close())
        return makeError (errc::io, "cannot finish " + partPath.string());

    const auto statsAfter = after.finish();

    // post-checks on the float data
    auto add = [&] (const std::string& check, const std::string& message, json details = nullptr)
    {
        const auto sev = severityFor (req, check);

        if (! sev.empty())
        {
            Finding f;
            f.check = check;
            f.severity = sev;
            f.name = out.trackName;
            f.message = message;
            f.details = std::move (details);
            res.postFindings.push_back (std::move (f));
        }
    };

    if (statsAfter.integratedLufs < -70.0 || statsAfter.samplePeakDbfs < -60.0)
        add ("silent_output", "the rendered audio is silent (integrated " + fmt (statsAfter.integratedLufs) + " LUFS, peak "
                                  + fmt (statsAfter.samplePeakDbfs) + " dBFS)");

    if (statsAfter.clippedSamples > req.clipTolerance)
        add ("clipping", std::to_string (statsAfter.clippedSamples) + " samples at or above 0 dBFS", json { { "clipped_samples", statsAfter.clippedSamples } });

    for (const char* set : { "fail", "warn" })
    {
        const auto& s = std::string (set) == "fail" ? req.failOn : req.warnOn;

        if (auto thr = thresholdOf (s, "true_peak_over:"); thr && statsAfter.truePeakDbtp > *thr + 0.05)
        {
            Finding f;
            f.check = "true_peak_over";
            f.severity = set;
            f.name = out.trackName;
            f.message = "true peak " + fmt (statsAfter.truePeakDbtp, 2) + " dBTP is over " + fmt (*thr);
            res.postFindings.push_back (std::move (f));
        }
    }

    json loudness { { "mode", req.loudness.mode } };

    if (req.loudness.mode != "none")
    {
        loudness["target_lufs"] = req.loudness.targetLufs;
        loudness["before_lufs"] = statsBefore.integratedLufs;
        loudness["after_lufs"] = statsAfter.integratedLufs;
        loudness["gain_db"] = gainDb;

        if (req.loudness.mode == "normalize")
        {
            loudness["limited_by_true_peak"] = limitedByPeak;
            loudness["true_peak_ceiling_dbtp"] = req.loudness.truePeakCeilingDb;
        }

        if (useLimiter)
            loudness["limiter"] = { { "enabled", true }, { "ceiling_dbtp", req.loudness.truePeakCeilingDb }, { "passes", limiterPasses },
                                    { "max_gain_reduction_db", std::round (limiterResult.maxGainReductionDb * 100.0) / 100.0 },
                                    { "limited_over_1db_percent", std::round (limiterResult.limitedOverOneDbPercent * 100.0) / 100.0 } };

        const bool onTarget = std::abs (statsAfter.integratedLufs - req.loudness.targetLufs) <= req.loudness.toleranceLu;
        loudness["on_target"] = onTarget;

        if (! onTarget)
        {
            Finding f;
            f.check = "loudness_target";
            f.severity = (req.loudness.mode == "check" || req.failOn.count ("loudness_target")) ? "fail" : "warn";
            f.name = out.trackName;
            f.message = "integrated loudness " + fmt (statsAfter.integratedLufs) + " LUFS is outside " + fmt (req.loudness.targetLufs) + " +- "
                        + fmt (req.loudness.toleranceLu) + " LU" + (limitedByPeak ? " (held back by the true-peak ceiling of " + fmt (req.loudness.truePeakCeilingDb) + " dBTP)" : std::string());
            res.postFindings.push_back (std::move (f));
        }
    }

    if (useLimiter && limiterResult.maxGainReductionDb > 6.0)
    {
        Finding f;
        f.check = "heavy_limiting";
        f.severity = "warn";
        f.name = out.trackName;
        f.message = "heavy limiting: gain reduction up to " + fmt (limiterResult.maxGainReductionDb, 1) + " dB ("
                    + fmt (limiterResult.limitedOverOneDbPercent, 1) + " % of the time over 1 dB)";
        res.postFindings.push_back (std::move (f));
    }

    res.failed = std::any_of (res.postFindings.begin(), res.postFindings.end(), [] (const Finding& f) { return f.severity == "fail"; });
    res.statsBefore = statsBefore.toJson();
    res.statsAfter = statsAfter.toJson();
    res.loudness = loudness;

    if (res.failed)
        return res;                 // temp files are deleted: no file is published

    if (cancelRequested())
        return makeError (errc::cancelled, "the render was cancelled");

    const auto finalPath = ctx.rendersDir / out.fileName;
    std::error_code ec;
    fs::rename (partPath, finalPath, ec);

    if (ec)
        return makeError (errc::io, "cannot publish " + finalPath.string() + ": " + ec.message());

    temps.keep (partPath);
    const auto sha = sha256File (finalPath);

    res.file = { { "path", "renders/" + out.fileName }, { "abs_path", finalPath.string() }, { "url", "/api/projects/" + ctx.projectId + "/renders/" + out.fileName },
                 { "sha256", sha.value_or ("") }, { "bytes", (std::int64_t) fs::file_size (finalPath, ec) }, { "duration_s", statsAfter.durationS },
                 { "container", spec.container }, { "sample_rate", sr }, { "channels", spec.channels } };
    return res;
}
} // namespace

Result<json> renderEdit (te::Edit& edit, const RenderContext& ctx, const RenderRequest& req)
{
    const double sr = req.format.sampleRate > 0 ? req.format.sampleRate : ctx.projectSampleRate;

    if (req.format.container != "wav" && req.format.container != "flac" && req.format.container != "mp3")
        return makeError (errc::invalidParams, "unsupported container " + req.format.container, json { { "path", "/format/container" } });

    auto range = resolveRange (edit, req.range);

    if (! range.ok())
        return range.error();

    std::error_code ec;
    fs::create_directories (ctx.rendersDir, ec);

    if (req.hooks.onProgress)
        req.hooks.onProgress (0.01, "preflight");

    auto preflight = runPreflight (edit, req, range.value(), sr, ctx.rendersDir);

    if (req.hooks.shouldCancel && req.hooks.shouldCancel())
        return makeError (errc::cancelled, "the render was cancelled");

    auto failure = [&] (const PreflightReport& rep, const char* phase) -> Error
    {
        json details = json::array();

        for (auto& f : rep.findings)
            if (f.severity == "fail")
                details.push_back (f.toJson());

        return makeError (errc::preflightFailed, std::string (phase) + " checks failed; no file was written", json { { "details", details }, { "preflight", rep.toJson() } });
    };

    if (! preflight.passed())
        return failure (preflight, "preflight");

    if (req.preflightOnly)
        return json { { "status", "preflight_passed" }, { "preflight", preflight.toJson() }, { "tracks", preflight.tracks } };

    primePlugins (edit, sr);

    // outputs
    std::vector<OneOutput> outputs;
    const auto ext = extensionOf (req.format.container);
    const auto base = sanitizeName (req.name);

    auto tracksBitset = [&] (const std::vector<std::string>& ids) -> Result<juce::BigInteger>
    {
        juce::Array<te::Track*> sel;

        for (auto& id : ids)
        {
            te::Track* found = nullptr;

            for (auto* t : te::getAllTracks (edit))
                if (Project::idOf (t->state) == id)
                    found = t;

            if (found == nullptr)
                return makeError (errc::notFound, "no track " + id, json { { "track_id", id } });

            sel.add (found);
        }

        return te::toBitSet (sel);
    };

    if (req.scope == "master")
    {
        outputs.push_back ({ base + "." + ext, "", {}, true });
    }
    else if (req.scope == "tracks" || req.scope == "stems")
    {
        if (req.trackIds.empty())
            return makeError (errc::invalidParams, "scope " + req.scope + " needs track_ids", json { { "path", "/scope/track_ids" } });

        if (req.scope == "tracks")
        {
            auto bits = tracksBitset (req.trackIds);

            if (! bits.ok())
                return bits.error();

            outputs.push_back ({ base + "." + ext, "tracks", bits.value(), true });
        }
        else
        {
            int n = 0;

            for (auto& id : req.trackIds)
            {
                auto bits = tracksBitset ({ id });

                if (! bits.ok())
                    return bits.error();

                std::string tname;

                for (auto* t : te::getAllTracks (edit))
                    if (Project::idOf (t->state) == id)
                        tname = toStd (t->getName());

                char idx[8];
                std::snprintf (idx, sizeof (idx), "%02d", ++n);
                outputs.push_back ({ base + "." + idx + "-" + sanitizeName (tname) + "." + ext, tname, bits.value(), req.stemsThroughMaster });
            }
        }
    }
    else
    {
        return makeError (errc::invalidParams, "unknown scope " + req.scope, json { { "path", "/scope" } });
    }

    TempFiles temps;
    json files = json::array(), stats = json::object(), statsBefore, loudness;
    PreflightReport all = preflight;

    int outputIndex = 0;

    for (auto& o : outputs)
    {
        if (req.hooks.shouldCancel && req.hooks.shouldCancel())
            return makeError (errc::cancelled, "the render was cancelled");

        o.index = outputIndex;
        o.progressFrom = 0.05 + 0.93 * (double) outputIndex / (double) outputs.size();
        o.progressTo = 0.05 + 0.93 * (double) (outputIndex + 1) / (double) outputs.size();
        ++outputIndex;
        auto r = produce (edit, ctx, req, o, range.value(), sr, temps);

        if (! r.ok())
            return r.error();

        for (auto& f : r.value().postFindings)
            all.findings.push_back (f);

        if (r.value().failed)
            return failure (all, "post-render");

        files.push_back (r.value().file);
        stats = r.value().statsAfter;
        statsBefore = r.value().statsBefore;
        loudness = r.value().loudness;
    }

    // metadata sidecar (task 160, "for embedding")
    json plugins = json::array();

    for (auto p : te::getAllPlugins (edit, false))
        if (auto* ext2 = dynamic_cast<te::ExternalPlugin*> (p))
            plugins.push_back ({ { "uid", toStd (ext2->desc.fileOrIdentifier) }, { "name", toStd (ext2->getName()) }, { "version", toStd (ext2->desc.version) } });

    std::unique_ptr<juce::XmlElement> xml (edit.state.createXml());
    Sha256 inputHash;
    const auto stateXml = xml->toString().toStdString();
    inputHash.update (stateXml.data(), stateXml.size());
    const std::string requestKey = req.scope + "|" + std::to_string (range.value().getStart().inSeconds()) + "|" + std::to_string (range.value().getEnd().inSeconds())
                                   + "|" + std::to_string (sr) + "|" + req.seed.value_or ("");
    inputHash.update (requestKey.data(), requestKey.size());

    json meta { { "engine", { { "rovox", rovoxVersion() }, { "tracktion", tracktionVersion() }, { "juce", toStd (juce::SystemStats::getJUCEVersion()) } } },
                { "source_revision", ctx.sourceRevision }, { "input_hash", inputHash.hexDigest() }, { "seed", req.seed ? json (*req.seed) : json (nullptr) },
                { "instrument_packs", json::array() }, { "plugins", plugins }, { "range_s", { range.value().getStart().inSeconds(), range.value().getEnd().inSeconds() } } };

    for (auto& f : files)
    {
        const auto sidecar = ctx.rendersDir / (fs::path (f["path"].get<std::string>()).stem().string() + ".render.json");
        auto m = meta;
        m["file"] = f;
        writeFileAtomic (sidecar, m.dump (2) + "\n", false);
    }

    json result { { "status", "succeeded" }, { "files", files }, { "file", files[0] }, { "source_revision", ctx.sourceRevision },
                  { "stats", stats }, { "loudness", loudness }, { "tracks", preflight.tracks }, { "preflight", all.toJson() }, { "metadata", meta } };

    if (loudness.contains ("gain_db") && std::abs (loudness["gain_db"].get<double>()) > 1e-9)
        result["stats_before_gain"] = statsBefore;

    return result;
}

std::unique_ptr<te::Edit> makeRenderSnapshot (Project& p)
{
    p.collector().beginScope();
    p.edit().flushState();
    p.collector().endScope();
    return createEditForProject (p.host().engine(), p.dir(), p.edit().state.createCopy(), te::Edit::EditRole::forRendering);
}

Result<json> renderProject (Project& p, const RenderRequest& req)
{
    auto snapshot = makeRenderSnapshot (p);
    RenderContext ctx;
    ctx.rendersDir = p.rendersDir();
    ctx.projectId = p.id();
    ctx.sourceRevision = p.revision();
    ctx.projectSampleRate = p.manifest().sampleRate;
    ++p.activeJobs();
    auto result = renderEdit (*snapshot, ctx, req);
    --p.activeJobs();
    return result;
}

} // namespace rovox
