// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Spike P1: Tracktion Engine headless (no audio device, no display).
// Generates two WAVs, builds an Edit with two audio tracks, renders 0-10 s offline to 48 kHz / 24-bit WAV,
// verifies duration and RMS, and exits through the JUCE message loop.
//
// usage: p1_headless <work-dir> [--hold]
//   --hold  after the render, keep the message loop alive until SIGTERM (clean-shutdown test)

#include <tracktion_engine/tracktion_engine.h>

#include <atomic>
#include <csignal>
#include <cstdio>

namespace te = tracktion;
using namespace te::literals;

namespace
{
constexpr double sampleRate = 48000.0;
constexpr double lengthSeconds = 10.0;
constexpr int numSamples = (int) (sampleRate * lengthSeconds);

constexpr float sineGain = 0.25118864f;  // -12 dBFS peak
constexpr float track1VolumeDb = -6.0f;
constexpr float track1Pan = 0.5f;        // linear pan law: L = g * (1 - pan), R = g * (1 + pan)

std::atomic<bool> sigtermReceived { false };
void onSigterm (int) { sigtermReceived = true; }

void writeMonoWav (const juce::File& file, const std::vector<float>& samples)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0));
    stream.release();   // writer owns the stream now

    juce::AudioBuffer<float> buffer (1, (int) samples.size());
    buffer.copyFrom (0, 0, samples.data(), (int) samples.size());
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

double rmsDb (double sumSquares)
{
    return 10.0 * std::log10 (std::max (sumSquares / numSamples, 1.0e-20));
}

struct Spike : private juce::Timer
{
    Spike (juce::File dir, bool holdAfterRender)
        : workDir (dir), hold (holdAfterRender)
    {
        startTimer (50);
    }

    ~Spike() override
    {
        stopTimer();
    }

    void run()
    {
        std::printf ("JUCE %s, Tracktion %s\n", juce::SystemStats::getJUCEVersion().toRawUTF8(), "engine");
        std::printf ("prefs folder: %s\n", engine.getPropertyStorage().getAppPrefsFolder().getFullPathName().toRawUTF8());
        std::printf ("audio device open: %s\n",
                     engine.getDeviceManager().deviceManager.getCurrentAudioDevice() != nullptr ? "yes" : "no");

        generateSources();
        buildEdit();
        startRender();
    }

    int exitCode = 1;

private:
    void timerCallback() override
    {
        if (sigtermReceived && ! shuttingDown)
        {
            shuttingDown = true;
            std::printf ("SIGTERM received, stopping message loop\n");
            juce::MessageManager::getInstance()->stopDispatchLoop();
        }
    }

    void generateSources()
    {
        sine.resize ((size_t) numSamples);
        click.resize ((size_t) numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            sine[(size_t) i] = sineGain * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sampleRate);

            // 2 ms burst at 0.5 amplitude every 0.5 s
            const int posInPeriod = i % (int) (sampleRate * 0.5);
            click[(size_t) i] = posInPeriod < (int) (sampleRate * 0.002) ? 0.5f : 0.0f;
        }

        sineFile = workDir.getChildFile ("sine440.wav");
        clickFile = workDir.getChildFile ("click.wav");
        writeMonoWav (sineFile, sine);
        writeMonoWav (clickFile, click);
    }

    void buildEdit()
    {
        edit = te::Edit::createSingleTrackEdit (engine, te::Edit::EditRole::forRendering);
        edit->ensureNumberOfAudioTracks (2);

        auto tracks = te::getAudioTracks (*edit);

        for (auto* t : tracks)
            t->getVolumePlugin()->setPanLaw (te::PanLawLinear);

        te::insertWaveClip (*tracks[0], {}, sineFile, { .time = { 0_tp, 10_td } }, te::DeleteExistingClips::no);
        te::insertWaveClip (*tracks[1], {}, clickFile, { .time = { 0_tp, 10_td } }, te::DeleteExistingClips::no);

        tracks[0]->getVolumePlugin()->setVolumeDb (track1VolumeDb);
        tracks[0]->getVolumePlugin()->setPan (track1Pan);
        tracks[1]->getVolumePlugin()->setVolumeDb (0.0f);
        tracks[1]->getVolumePlugin()->setPan (0.0f);
    }

    void startRender()
    {
        outFile = workDir.getChildFile ("out.wav");
        outFile.deleteFile();

        te::Renderer::Parameters params (*edit);
        params.destFile = outFile;
        params.audioFormat = engine.getAudioFileFormatManager().getWavFormat();
        params.bitDepth = 24;
        params.blockSizeForAudio = 512;
        params.sampleRateForAudio = sampleRate;
        params.time = { 0_tp, 10_td };
        params.canRenderInMono = false;

        renderStartMs = juce::Time::getMillisecondCounterHiRes();

        handle = te::EditRenderer::render (std::move (params), [this] (auto result)
        {
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - renderStartMs;
            renderMs = elapsed;
            renderOk = (bool) result;

            if (! result)
                std::printf ("render error: %s\n", result.error().c_str());

            juce::MessageManager::callAsync ([this] { onRenderFinished(); });
        });
    }

    void onRenderFinished()
    {
        std::printf ("render wall time: %.1f ms (%.1fx realtime)\n", renderMs, lengthSeconds * 1000.0 / renderMs);
        exitCode = renderOk && verifyOutput() ? 0 : 1;

        if (! hold)
            juce::MessageManager::getInstance()->stopDispatchLoop();
        else
            std::printf ("holding message loop until SIGTERM\n");
    }

    bool verifyOutput()
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (outFile));

        if (reader == nullptr)
        {
            std::printf ("output not readable\n");
            return false;
        }

        std::printf ("output: %lld samples @ %.0f Hz, %u ch, %u bit, duration %.6f s\n",
                     (long long) reader->lengthInSamples, reader->sampleRate, reader->numChannels,
                     reader->bitsPerSample, (double) reader->lengthInSamples / reader->sampleRate);

        if (reader->numChannels != 2 || reader->lengthInSamples < numSamples - 512 || reader->lengthInSamples > numSamples + 512)
            return false;

        juce::AudioBuffer<float> out (2, numSamples);
        reader->read (&out, 0, numSamples, 0, true, true);

        const float g1 = std::pow (10.0f, track1VolumeDb / 20.0f);
        const float gL = g1 * (1.0f - track1Pan);
        const float gR = g1 * (1.0f + track1Pan);

        double expL = 0, expR = 0, actL = 0, actR = 0;

        for (int i = 0; i < numSamples; ++i)
        {
            const double eL = gL * sine[(size_t) i] + click[(size_t) i];
            const double eR = gR * sine[(size_t) i] + click[(size_t) i];
            expL += eL * eL;
            expR += eR * eR;
            actL += (double) out.getSample (0, i) * out.getSample (0, i);
            actR += (double) out.getSample (1, i) * out.getSample (1, i);
        }

        const double dL = rmsDb (actL) - rmsDb (expL);
        const double dR = rmsDb (actR) - rmsDb (expR);

        std::printf ("RMS L: actual %.3f dBFS, expected %.3f dBFS, diff %+.3f dB\n", rmsDb (actL), rmsDb (expL), dL);
        std::printf ("RMS R: actual %.3f dBFS, expected %.3f dBFS, diff %+.3f dB\n", rmsDb (actR), rmsDb (expR), dR);

        return std::abs (dL) <= 0.5 && std::abs (dR) <= 0.5;
    }

    juce::File workDir, sineFile, clickFile, outFile;
    bool hold = false, shuttingDown = false, renderOk = false;
    double renderStartMs = 0, renderMs = 0;
    std::vector<float> sine, click;

public:
    // Declaration order matters for destruction: handle -> edit -> engine
    te::Engine engine { "rovox_p1_headless" };
    std::unique_ptr<te::Edit> edit;
    std::shared_ptr<te::EditRenderer::Handle> handle;

    void shutdown()
    {
        handle.reset();
        edit.reset();
    }
};
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: p1_headless <work-dir> [--hold]\n");
        return 2;
    }

    std::signal (SIGTERM, onSigterm);
    std::signal (SIGINT, onSigterm);

    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File workDir (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    workDir.createDirectory();

    int exitCode = 1;

    {
        Spike spike (workDir, argc > 2 && juce::String (argv[2]) == "--hold");
        juce::MessageManager::callAsync ([&spike] { spike.run(); });

        juce::MessageManager::getInstance()->runDispatchLoop();

        spike.shutdown();
        exitCode = spike.exitCode;
        std::printf ("message loop exited, exit code %d\n", exitCode);
    }

    return exitCode;
}
