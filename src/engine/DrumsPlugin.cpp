// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "DrumsPlugin.h"

#include "DrumKit.h"
#include "DrumKitLibrary.h"
#include "Sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace rovox
{

namespace
{
enum class Phase { resolving, found, loading, ready, failed };

bool sameRate (double a, double b)      { return std::abs (a - b) < 0.5; }

const char* phaseName (Phase p)
{
    switch (p)
    {
        case Phase::resolving: return "resolving";
        case Phase::found:     return "loading";
        case Phase::loading:   return "loading";
        case Phase::ready:     return "ready";
        case Phase::failed:    return "error";
    }

    return "error";
}

/** Identity of a kit: kit.json plus, for every sample it lists (sorted), the relative path, size and sha256 of the bytes. The per-file hashes are
    cached by path + size + mtime, so reloading an unchanged kit (another sample rate, another plugin instance) does not read the samples again. */
std::string kitFingerprint (const fs::path& dir, const std::string& kitText, std::vector<std::string> samples)
{
    static std::mutex cacheMutex;
    static std::map<std::string, std::string> cache;

    Sha256 h;
    h.update (kitText.data(), kitText.size());
    std::sort (samples.begin(), samples.end());

    for (auto& rel : samples)
    {
        std::error_code sizeEc, timeEc;
        const auto file = dir / rel;
        const auto size = fs::file_size (file, sizeEc);
        const auto time = fs::last_write_time (file, timeEc).time_since_epoch();
        const auto key = file.string() + "|" + std::to_string (sizeEc ? 0 : size) + "|" + std::to_string (timeEc ? 0 : time.count());
        std::string fileHash;
        {
            std::lock_guard<std::mutex> lock (cacheMutex);
            if (auto it = cache.find (key); it != cache.end())
                fileHash = it->second;
        }

        if (fileHash.empty())
        {
            fileHash = sha256File (file).value_or ("unreadable");
            std::lock_guard<std::mutex> lock (cacheMutex);
            cache[key] = fileHash;
        }

        const auto line = "\n" + rel + "|" + std::to_string (sizeEc ? 0 : size) + "|" + fileHash;
        h.update (line.data(), line.size());
    }

    return h.hexDigest();
}

/** Decodes one audio file to stereo 16-bit at `rate`. */
Result<std::shared_ptr<DrumSampleData>> decodeSample (juce::AudioFormatManager& formats, const fs::path& file, double rate)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (toFile (file)));

    if (reader == nullptr || reader->lengthInSamples <= 0)
        return makeError (errc::io, "cannot decode " + file.string());

    const int length = (int) std::min<juce::int64> (reader->lengthInSamples, 60 * 48000);
    juce::AudioBuffer<float> in (2, length);
    in.clear();

    if (! reader->read (&in, 0, length, 0, true, true))
        return makeError (errc::io, "cannot read " + file.string());

    if (reader->numChannels == 1)
        in.copyFrom (1, 0, in, 0, 0, length);

    if (std::abs (reader->sampleRate - rate) < 0.5)
        return DrumSampleData::fromFloat (in.getReadPointer (0), in.getReadPointer (1), (size_t) length);

    const double ratio = reader->sampleRate / rate;
    const int outLength = std::max (1, (int) std::ceil ((double) length / ratio));
    juce::AudioBuffer<float> out (2, outLength);
    out.clear();

    for (int c = 0; c < 2; ++c)
    {
        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio, in.getReadPointer (c), out.getWritePointer (c), outLength);
    }

    return DrumSampleData::fromFloat (out.getReadPointer (0), out.getReadPointer (1), (size_t) outLength);
}

//==============================================================================
class RovoxDrums : public te::Plugin
{
public:
    static inline const char* xmlTypeName = drumsXmlType;

    RovoxDrums (te::PluginCreationInfo info) : te::Plugin (info), kitListener_ (*this)
    {
        auto* um = getUndoManager();
        gain_.referTo (state, "gain", um, 0.0f);
        gainParam_ = addParam ("gain", "Output gain", juce::NormalisableRange<float> (-48.0f, 12.0f),
                               [] (float v) { return juce::String (v, 1) + " dB"; },
                               [] (const juce::String& s) { return s.getFloatValue(); });
        gainParam_->attachToCurrentValue (gain_);
        state.addListener (&kitListener_);
        startWorker();
    }

    ~RovoxDrums() override
    {
        state.removeListener (&kitListener_);
        notifyListenersOfDeletion();
        stopWorker();
        gainParam_->detachFromCurrentValue();
    }

    juce::String getName() const override                   { return "Rovox Drums"; }
    juce::String getPluginType() override                   { return xmlTypeName; }
    juce::String getVendor() override                       { return "Rovox"; }
    juce::String getShortName (int) override                { return "Drums"; }
    juce::String getSelectableDescription() override        { return getName(); }
    int getNumOutputChannelsGivenInputs (int) override      { return 2; }
    BusLayout getBusses() const override                    { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                          { return true; }
    bool isSynth() override                                 { return true; }
    bool producesAudioWhenNoAudioInput() override           { return true; }
    bool noTail() override                                  { return false; }
    double getTailLength() const override                   { return 2.0; }
    void deinitialise() override                            {}

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        {
            std::lock_guard<std::mutex> lock (mutex_);
            requestedRate_ = info.sampleRate;
            requestedRateA_ = info.sampleRate;
            blockSize_ = std::max (1, info.blockSizeSamples);
        }

        scratchL_.assign ((size_t) blockSize_, 0.0f);
        scratchR_.assign ((size_t) blockSize_, 0.0f);
        events_.reserve (1024);
        cv_.notify_all();
    }

    void reset() override
    {
        std::unique_lock<std::mutex> lock (samplerMutex_, std::try_to_lock);

        if (lock.owns_lock() && sampler_ != nullptr)
            sampler_->allNotesOff();
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
            return;

        for (int c = 0; c < fc.destBuffer->getNumChannels(); ++c)
            fc.destBuffer->clear (c, fc.bufferStartSample, fc.bufferNumSamples);

        if (fc.isRendering && ! failed_ && ! sameRate (loadedRate_.load(), requestedRateA_.load()))
            waitUntilSettled (std::chrono::seconds (30));

        std::unique_lock<std::mutex> lock (samplerMutex_, std::try_to_lock);

        if (! lock.owns_lock() || sampler_ == nullptr)
            return;

        juce::ScopedNoDenormals noDenormals;
        const int total = fc.bufferNumSamples;

        for (int done = 0; done < total;)
        {
            const int count = std::min (total - done, (int) scratchL_.size());
            collectEvents (fc, done, count);
            std::fill (scratchL_.begin(), scratchL_.begin() + count, 0.0f);
            std::fill (scratchR_.begin(), scratchR_.begin() + count, 0.0f);
            sampler_->render (scratchL_.data(), scratchR_.data(), count, events_.data(), (int) events_.size());
            const float g = std::pow (10.0f, gainParam_->getCurrentValue() / 20.0f);

            if (fc.destBuffer->getNumChannels() >= 2)
            {
                float* l = fc.destBuffer->getWritePointer (0, fc.bufferStartSample + done);
                float* r = fc.destBuffer->getWritePointer (1, fc.bufferStartSample + done);

                for (int i = 0; i < count; ++i)
                {
                    l[i] = scratchL_[(size_t) i] * g;
                    r[i] = scratchR_[(size_t) i] * g;
                }
            }
            else
            {
                float* m = fc.destBuffer->getWritePointer (0, fc.bufferStartSample + done);

                for (int i = 0; i < count; ++i)
                    m[i] = 0.5f * (scratchL_[(size_t) i] + scratchR_[(size_t) i]) * g;
            }

            done += count;
        }
    }

    json extras (double waitS)
    {
        std::unique_lock<std::mutex> lock (mutex_);

        if (waitS > 0.0)
            cv_.wait_for (lock, std::chrono::duration<double> (waitS), [this] { return phase_ != Phase::resolving; });

        json j { { "kit", kitId_ }, { "status", phaseName (phase_) }, { "message", message_ }, { "attribution", drumsAttribution } };

        if (phase_ == Phase::failed)
            j["error"] = message_;

        if (! source_.empty())
            j["source"] = source_;

        if (manifest_)
        {
            j["name"] = manifest_->name;
            j["license"] = manifest_->license;
            j["kit_attribution"] = manifest_->attribution;
            j["source_url"] = manifest_->source;
            j["pieces"] = manifest_->pieces.size();
            j["samples"] = manifest_->samplePaths().size();
            j["kit_id"] = manifest_->id;
            j["kit_version"] = manifest_->version;
            j["kit_sha256"] = kitSha_;
        }

        lock.unlock();
        std::lock_guard<std::mutex> samplerLock (samplerMutex_);
        j["unmapped_notes"] = sampler_ != nullptr ? sampler_->unmappedNotes() : json::object();
        return j;
    }

private:
    /** Restarts the worker when the "kit" property changes. */
    struct KitListener : juce::ValueTree::Listener
    {
        explicit KitListener (RovoxDrums& o) : owner (o) {}

        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& id) override
        {
            if (tree == owner.state && id == juce::Identifier ("kit") && owner.currentKit() != owner.loadedKit_)
            {
                owner.stopWorker();
                owner.startWorker();
            }
        }

        RovoxDrums& owner;
    };

    juce::String currentKit() const
    {
        const auto v = state.getProperty ("kit", juce::String (defaultDrumKit)).toString();
        return v.isEmpty() ? juce::String (defaultDrumKit) : v;
    }

    void collectEvents (const te::PluginRenderContext& fc, int from, int count)
    {
        events_.clear();

        if (fc.bufferForMidiMessages == nullptr)
            return;

        if (fc.bufferForMidiMessages->isAllNotesOff && from == 0)
            sampler_->allNotesOff();

        const double rate = loadedRate_.load();

        for (auto& m : *fc.bufferForMidiMessages)
        {
            if (! m.isNoteOn())
                continue;

            // time stamps are seconds from the start of the block; a stamp that rounds up to its end is still inside
            const int pos = std::clamp (juce::roundToInt ((m.getTimeStamp() + fc.midiBufferOffset) * rate), 0, fc.bufferNumSamples - 1);

            if (pos >= from && pos < from + count)
                events_.push_back ({ pos - from, m.getNoteNumber(), (int) m.getVelocity() });
        }

        // insertion sort: stable and allocation free (blocks hold a handful of events)
        for (size_t i = 1; i < events_.size(); ++i)
            for (size_t j = i; j > 0 && events_[j - 1].offset > events_[j].offset; --j)
                std::swap (events_[j - 1], events_[j]);
    }

    /** Offline renders wait here until the kit is decoded (or has failed): the first block must already sound. */
    void waitUntilSettled (std::chrono::seconds limit)
    {
        std::unique_lock<std::mutex> lock (mutex_);
        const auto deadline = std::chrono::steady_clock::now() + limit;
        const auto settled = [this] { return quit_ || phase_ == Phase::failed || (phase_ == Phase::ready && sameRate (loadedRateD_, requestedRate_)); };

        // short slices so that a cancelled or stalled render (which signals its job) is not held on the render thread
        while (! settled() && std::chrono::steady_clock::now() < deadline)
        {
            if (juce::Thread::currentThreadShouldExit())
                return;

            if (auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob(); job != nullptr && job->shouldExit())
                return;

            cv_.wait_for (lock, std::chrono::milliseconds (100), settled);
        }
    }

    void startWorker()
    {
        loadedKit_ = currentKit();
        {
            std::lock_guard<std::mutex> lock (mutex_);
            quit_ = false;
            kitId_ = toStd (loadedKit_);
            phase_ = Phase::resolving;
            message_ = "looking for the drum kit";
            manifest_.reset();
            source_.clear();
            loadedRateD_ = 0.0;
        }
        failed_ = false;
        loadedRate_ = 0.0;
        worker_ = std::thread ([this, id = toStd (loadedKit_)] { workerMain (id); });
    }

    void stopWorker()
    {
        {
            std::lock_guard<std::mutex> lock (mutex_);
            quit_ = true;
        }
        cv_.notify_all();

        if (worker_.joinable())
            worker_.join();

        std::lock_guard<std::mutex> lock (samplerMutex_);
        sampler_.reset();
    }

    bool isQuitting()
    {
        std::lock_guard<std::mutex> lock (mutex_);
        return quit_;
    }

    void setPhase (Phase p, const std::string& message)
    {
        {
            std::lock_guard<std::mutex> lock (mutex_);
            phase_ = p;
            message_ = message;
            failed_ = p == Phase::failed;
        }
        cv_.notify_all();
    }

    void workerMain (const std::string& kitId)
    {
        auto location = DrumKitLibrary::instance().resolve (kitId, [this] (const std::string& m) { if (! isQuitting()) setPhase (Phase::resolving, m); });

        if (! location.ok())
            return setPhase (Phase::failed, location.error().message);

        std::ifstream in (location.value().dir / "kit.json", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const auto kitText = text.str();
        auto parsed = json::parse (kitText, nullptr, false);

        if (parsed.is_discarded())
            return setPhase (Phase::failed, "drum kit " + kitId + ": kit.json is not valid JSON (" + location.value().dir.string() + ")");

        auto manifest = DrumKitManifest::fromJson (parsed);

        if (! manifest.ok())
            return setPhase (Phase::failed, manifest.error().message);

        const auto dir = location.value().dir;
        const auto fingerprint = kitFingerprint (dir, kitText, manifest.value().samplePaths());       // hashes every sample once (cached), outside the lock
        {
            std::lock_guard<std::mutex> lock (mutex_);
            manifest_ = manifest.value();
            source_ = location.value().source;
            kitSha_ = fingerprint;
        }
        setPhase (Phase::found, "decoding the samples");

        // decode for every sample rate the engine asks for (usually once)
        double doneRate = 0.0;

        while (! isQuitting())
        {
            double rate = 0.0;
            {
                std::unique_lock<std::mutex> lock (mutex_);
                cv_.wait (lock, [&] { return quit_ || (requestedRate_ > 0.0 && ! sameRate (requestedRate_, doneRate)); });

                if (quit_)
                    return;

                rate = requestedRate_;
            }

            auto kit = DrumKit::create (manifest.value());

            if (! kit.ok())
                return setPhase (Phase::failed, kit.error().message);

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            DrumSampleMap samples;
            const auto paths = manifest.value().samplePaths();
            size_t n = 0;

            for (auto& rel : paths)
            {
                if (isQuitting())
                    return;

                auto decoded = decodeSample (formats, dir / rel, rate);

                if (! decoded.ok())
                    return setPhase (Phase::failed, "drum kit " + kitId + ": " + decoded.error().message);

                samples[rel] = decoded.value();

                if (++n % 50 == 0)
                    setPhase (Phase::found, "decoded " + std::to_string (n) + " of " + std::to_string (paths.size()) + " samples");
            }

            auto sampler = std::make_unique<DrumSampler> (rate);

            if (auto r = sampler->setKit (std::move (kit.value()), std::move (samples)); ! r.ok())
                return setPhase (Phase::failed, r.error().message);

            {
                std::lock_guard<std::mutex> lock (samplerMutex_);
                sampler_ = std::move (sampler);
            }
            doneRate = rate;
            {
                std::lock_guard<std::mutex> lock (mutex_);
                loadedRateD_ = rate;
                loadedRate_ = rate;
                phase_ = Phase::ready;
                message_ = "ready";
            }
            cv_.notify_all();
        }
    }

    juce::CachedValue<float> gain_;
    te::AutomatableParameter::Ptr gainParam_;
    KitListener kitListener_;
    juce::String loadedKit_;

    std::mutex mutex_;                  // phase, message, manifest, rates
    std::condition_variable cv_;
    Phase phase_ = Phase::resolving;
    std::string message_, source_, kitSha_;
    std::optional<DrumKitManifest> manifest_;
    double requestedRate_ = 44100.0, loadedRateD_ = 0.0;      // decoded for 44.1 kHz at once, again if the engine runs at another rate
    std::atomic<double> requestedRateA_ { 0.0 }, loadedRate_ { 0.0 };       // for the audio thread
    std::atomic<bool> failed_ { false };
    std::string kitId_;
    int blockSize_ = 512;
    std::thread worker_;
    bool quit_ = false;          // under mutex_

    std::mutex samplerMutex_;           // the audio thread only try-locks it
    std::unique_ptr<DrumSampler> sampler_;
    std::vector<float> scratchL_, scratchR_;
    std::vector<DrumNoteEvent> events_;
};
} // namespace

void registerDrumsPlugin (te::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<RovoxDrums>();
}

json drumsPluginExtras (te::Plugin& p, double waitS)
{
    if (auto* d = dynamic_cast<RovoxDrums*> (&p))
        return d->extras (waitS);

    return nullptr;
}

json drumsKitIdentity (te::Plugin& p, double waitS)
{
    auto extras = drumsPluginExtras (p, waitS);

    if (! extras.is_object() || ! extras.contains ("kit_sha256"))
        return nullptr;

    return { { "id", extras["kit_id"] }, { "version", extras["kit_version"] }, { "sha256", extras["kit_sha256"] },
             { "license", extras["license"] }, { "source", extras["source_url"] } };
}

} // namespace rovox
