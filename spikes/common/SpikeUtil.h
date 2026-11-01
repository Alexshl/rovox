// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Shared helpers for the throwaway spikes P2..P5 (WAV IO, level maths, cgroup readers, blocking render).
#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include <sys/resource.h>

namespace spike
{
namespace te = tracktion;

inline double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli> (steady_clock::now().time_since_epoch()).count();
}

/** Peak resident set size of this process in kB (Linux ru_maxrss unit). */
inline long peakRssKb()
{
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
    return (long) ru.ru_maxrss;
}

inline juce::String readSmallFile (const char* path)
{
    return juce::File (path).loadFileAsString().trim();
}

/** CPU quota from cgroup v2 cpu.max ("max 100000" or "200000 100000"), 0 if unlimited/unknown. */
inline double cgroupCpuQuota()
{
    const auto s = readSmallFile ("/sys/fs/cgroup/cpu.max");
    juce::StringArray t;
    t.addTokens (s, " ", "");

    if (t.size() == 2 && t[0] != "max" && t[1].getIntValue() > 0)
        return (double) t[0].getLargeIntValue() / (double) t[1].getLargeIntValue();

    return 0.0;
}

/** Memory limit in bytes from cgroup v2, 0 if unlimited/unknown. */
inline juce::int64 cgroupMemoryLimit()
{
    const auto s = readSmallFile ("/sys/fs/cgroup/memory.max");
    return (s.isEmpty() || s == "max") ? 0 : s.getLargeIntValue();
}

inline juce::int64 cgroupMemoryPeak()   { return readSmallFile ("/sys/fs/cgroup/memory.peak").getLargeIntValue(); }

/** Worker thread count the audio graph should use: ceil(cgroup quota), else the host CPU count. */
inline int cpusAvailableToUs()
{
    const auto q = cgroupCpuQuota();
    return q > 0.0 ? juce::jmax (1, (int) std::ceil (q)) : juce::jmax (1, juce::SystemStats::getNumCpus());
}

/** Engine behaviour: honours the cgroup CPU quota, optional out-of-process plugin scan, optional device-less engine. */
struct SpikeBehaviour : te::EngineBehaviour
{
    int getNumberOfCPUsToUseForAudio() override     { return useCgroup ? cpusAvailableToUs() : EngineBehaviour::getNumberOfCPUsToUseForAudio(); }
    bool canScanPluginsOutOfProcess() override      { return scanOutOfProcess; }
    bool addSystemAudioIODeviceTypes() override     { return systemDevices; }
    bool autoInitialiseDeviceManager() override     { return autoInit; }
    te::EditLimits getEditLimits() override         { return limits; }

    te::EditLimits limits;      // Tracktion's default is 400 tracks per Edit; insertTrack() silently returns null beyond it

    bool useCgroup = true, scanOutOfProcess = false, systemDevices = true, autoInit = true;
};

inline double toDb (double lin)  { return 20.0 * std::log10 (std::max (lin, 1.0e-10)); }

inline double rmsDb (const float* d, int n)
{
    if (n <= 0)
        return -200.0;

    double s = 0;

    for (int i = 0; i < n; ++i)
        s += (double) d[i] * d[i];

    return 10.0 * std::log10 (std::max (s / n, 1.0e-20));
}

struct Wav
{
    double sampleRate = 0;
    std::vector<std::vector<float>> ch;
    int numSamples() const { return ch.empty() ? 0 : (int) ch[0].size(); }
};

inline void writeWav (const juce::File& file, const std::vector<std::vector<float>>& channels, double sampleRate, int bits = 24)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, (unsigned) channels.size(), bits, {}, 0));
    stream.release();

    juce::AudioBuffer<float> buffer ((int) channels.size(), (int) channels[0].size());

    for (size_t c = 0; c < channels.size(); ++c)
        buffer.copyFrom ((int) c, 0, channels[c].data(), (int) channels[c].size());

    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

inline bool readWav (const juce::File& file, Wav& out)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));

    if (r == nullptr)
        return false;

    juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    out.sampleRate = r->sampleRate;
    out.ch.assign (r->numChannels, {});

    for (unsigned c = 0; c < r->numChannels; ++c)
        out.ch[c].assign (b.getReadPointer ((int) c), b.getReadPointer ((int) c) + b.getNumSamples());

    return true;
}

/** Offline render on the calling thread (no message loop needed). Returns an error string, empty on success. */
inline juce::String renderBlocking (te::Edit& edit, te::Renderer::Parameters params, float* peakOut = nullptr, float* rmsOut = nullptr)
{
    const te::Edit::ScopedRenderStatus srs (edit, true);
    te::TransportControl::stopAllTransports (edit.engine, false, true);

    params.destFile.deleteFile();
    te::Renderer::RenderTask task ("spike", params, nullptr, nullptr);

    if (task.errorMessage.isNotEmpty())
        return task.errorMessage;

    while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
    {}

    if (peakOut != nullptr)  *peakOut = task.params.resultMagnitude;
    if (rmsOut != nullptr)   *rmsOut = task.params.resultRMS;

    if (task.errorMessage.isNotEmpty())
        return task.errorMessage;

    return params.destFile.existsAsFile() ? juce::String() : juce::String ("no output file");
}

inline te::Renderer::Parameters makeRenderParams (te::Edit& edit, const juce::File& dest, double sampleRate, double seconds, int blockSize = 512)
{
    te::Renderer::Parameters p (edit);
    p.destFile = dest;
    p.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
    p.bitDepth = 24;
    p.blockSizeForAudio = blockSize;
    p.sampleRateForAudio = sampleRate;
    p.time = { te::TimePosition::fromSeconds (0.0), te::TimeDuration::fromSeconds (seconds) };
    p.canRenderInMono = false;
    return p;
}

inline juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();

    for (auto& p : props)
        o->setProperty (p.first, p.second);

    return juce::var (o);
}
}
