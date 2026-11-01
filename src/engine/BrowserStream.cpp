// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "BrowserStream.h"

#include <opus/opus.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>

namespace rovox
{

namespace
{
constexpr int kRate = BrowserStream::sampleRate;
constexpr int kBlock = BrowserStream::blockSize;
constexpr int kFrame = BrowserStream::frameSize;

struct Block
{
    float l[kBlock], r[kBlock];
    std::uint64_t samplePos = 0;
    std::uint16_t epoch = 0;
    bool playing = false;
};

struct ClockStats
{
    std::atomic<std::int64_t> maxLateUs { 0 }, lateUsTotal { 0 }, ticks { 0 }, lateOver2ms { 0 }, lateOver5ms { 0 };
    std::atomic<std::int64_t> callbackUsTotal { 0 }, callbackUsMax { 0 };
};

double nowMs()
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** AudioIODevice driven by a high-priority thread that sleeps until absolute steady_clock deadlines (no accumulating drift). */
class StreamDevice : public juce::AudioIODevice, private juce::Thread
{
public:
    using Sink = std::function<void (const float* l, const float* r, int n)>;

    StreamDevice (Sink s, ClockStats& st) : juce::AudioIODevice ("Browser Stream", "BrowserStream"), juce::Thread ("rovox stream clock"), sink (std::move (s)), stats (st) {}
    ~StreamDevice() override { stop(); }

    juce::StringArray getOutputChannelNames() override          { return { "Stream L", "Stream R" }; }
    juce::StringArray getInputChannelNames() override           { return {}; }
    juce::Array<double> getAvailableSampleRates() override      { return { (double) kRate }; }
    juce::Array<int> getAvailableBufferSizes() override         { return { kBlock }; }
    int getDefaultBufferSize() override                         { return kBlock; }

    juce::String open (const juce::BigInteger&, const juce::BigInteger& outputs, double, int) override
    {
        activeOutputs = outputs;
        activeOutputs.setRange (0, 2, true);
        opened = true;
        return {};
    }

    void close() override                       { stop(); opened = false; }
    bool isOpen() override                      { return opened; }

    void start (juce::AudioIODeviceCallback* cb) override
    {
        stop();
        cb->audioDeviceAboutToStart (this);
        {
            const juce::ScopedLock sl (lock);
            callback = cb;
        }
        startThread (juce::Thread::Priority::highest);
    }

    void stop() override
    {
        stopThread (2000);
        juce::AudioIODeviceCallback* old = nullptr;
        {
            const juce::ScopedLock sl (lock);
            std::swap (old, callback);
        }

        if (old != nullptr)
            old->audioDeviceStopped();
    }

    bool isPlaying() override                   { return isThreadRunning(); }
    juce::String getLastError() override        { return {}; }
    int getCurrentBufferSizeSamples() override  { return kBlock; }
    double getCurrentSampleRate() override      { return kRate; }
    int getCurrentBitDepth() override           { return 32; }
    juce::BigInteger getActiveOutputChannels() const override   { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override    { return {}; }
    int getOutputLatencyInSamples() override    { return 0; }
    int getInputLatencyInSamples() override     { return 0; }

private:
    void run() override
    {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds ((long long) (1.0e9 * kBlock / kRate));
        auto next = clock::now();
        juce::AudioBuffer<float> out (2, kBlock);
        const float* const* noInputs = nullptr;

        while (! threadShouldExit())
        {
            next += period;
            std::this_thread::sleep_until (next);
            const auto late = std::chrono::duration_cast<std::chrono::microseconds> (clock::now() - next).count();
            stats.ticks++;
            stats.lateUsTotal += late;

            if (late > stats.maxLateUs)
                stats.maxLateUs = late;

            if (late > 2000)
                stats.lateOver2ms++;

            if (late > 5000)
                stats.lateOver5ms++;

            out.clear();
            float* const outs[2] = { out.getWritePointer (0), out.getWritePointer (1) };
            const auto c0 = clock::now();

            {
                const juce::ScopedLock sl (lock);

                // the device starts delivering callbacks inside DeviceManager::initialise(), before any Edit exists
                if (callback != nullptr)
                    callback->audioDeviceIOCallbackWithContext (noInputs, 0, outs, 2, kBlock, {});
            }

            const auto cb = std::chrono::duration_cast<std::chrono::microseconds> (clock::now() - c0).count();
            stats.callbackUsTotal += cb;

            if (cb > stats.callbackUsMax)
                stats.callbackUsMax = cb;

            sink (outs[0], outs[1], kBlock);
        }
    }

    Sink sink;
    ClockStats& stats;
    juce::CriticalSection lock;
    juce::AudioIODeviceCallback* callback = nullptr;
    juce::BigInteger activeOutputs;
    bool opened = false;
};

class StreamDeviceType : public juce::AudioIODeviceType
{
public:
    StreamDeviceType (StreamDevice::Sink s, ClockStats& st) : juce::AudioIODeviceType ("BrowserStream"), sink (std::move (s)), stats (st) {}

    void scanForDevices() override                          {}
    juce::StringArray getDeviceNames (bool) const override  { return { "Browser Stream" }; }
    int getDefaultDeviceIndex (bool) const override         { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* d, bool) const override { return d != nullptr ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override       { return false; }

    juce::AudioIODevice* createDevice (const juce::String& out, const juce::String&) override
    {
        return out.isEmpty() || out == "Browser Stream" ? new StreamDevice (sink, stats) : nullptr;
    }

private:
    StreamDevice::Sink sink;
    ClockStats& stats;
};

/** Lock-free single-producer / single-consumer ring of device blocks. */
class BlockRing
{
public:
    BlockRing() : fifo (512), blocks (512) {}

    bool push (const Block& b)
    {
        int s1, z1, s2, z2;
        fifo.prepareToWrite (1, s1, z1, s2, z2);

        if (z1 == 0)
            return false;

        blocks[(size_t) s1] = b;
        fifo.finishedWrite (1);
        return true;
    }

    bool pop (Block& b)
    {
        int s1, z1, s2, z2;
        fifo.prepareToRead (1, s1, z1, s2, z2);

        if (z1 == 0)
            return false;

        b = blocks[(size_t) s1];
        fifo.finishedRead (1);
        return true;
    }

private:
    juce::AbstractFifo fifo;
    std::vector<Block> blocks;
};
} // namespace

//==============================================================================
struct BrowserStream::Impl
{
    Impl (BrowserStream& o, te::Engine& e, int bitrate) : owner (o), engine (e)
    {
        int err = 0;
        encoder = opus_encoder_create (kRate, 2, OPUS_APPLICATION_AUDIO, &err);
        opus_encoder_ctl (encoder, OPUS_SET_BITRATE (bitrate));
        opus_encoder_ctl (encoder, OPUS_SET_COMPLEXITY (5));
    }

    ~Impl()
    {
        quit = true;
        pending.signal();

        if (encoderThread.joinable())
            encoderThread.join();

        opus_encoder_destroy (encoder);
    }

    // device clock thread
    void onBlock (const float* l, const float* r, int n)
    {
        Block b;
        std::memcpy (b.l, l, sizeof (float) * (size_t) n);
        std::memcpy (b.r, r, sizeof (float) * (size_t) n);
        b.playing = owner.playingProjects_.load() > 0;
        b.epoch = owner.epoch_.load();

        if (b.playing)
        {
            std::lock_guard<std::mutex> lock (sourceMutex);

            if (source != nullptr)
                if (auto* ctx = source->getTransport().getCurrentPlaybackContext())
                    b.samplePos = (std::uint64_t) std::max (0.0, ctx->getPosition().inSeconds() * kRate);
        }

        if (! ring.push (b))
            ringOverflows++;

        pending.signal();
    }

    // encoder thread
    void encoderLoop()
    {
        std::array<float, kFrame * 2> pcm {};
        int filled = 0;
        std::uint16_t frameEpoch = 0, lastEpochSent = 0xffff;
        std::uint64_t framePos = 0, lastPacketEndPos = 0;
        bool haveLast = false;
        std::uint32_t seq = 0;
        std::vector<std::uint8_t> opusBuf (1500), packet;

        while (! quit)
        {
            pending.wait (20);
            Block b;

            while (ring.pop (b))
            {
                if (! b.playing)
                {
                    filled = 0;
                    haveLast = false;
                    continue;
                }

                if (filled > 0 && b.epoch != frameEpoch)
                    filled = 0;

                if (filled == 0)
                {
                    frameEpoch = b.epoch;
                    framePos = b.samplePos;
                }

                for (int i = 0; i < kBlock; ++i)
                {
                    pcm[(size_t) (filled + i) * 2] = b.l[i];
                    pcm[(size_t) (filled + i) * 2 + 1] = b.r[i];
                }

                filled += kBlock;

                if (filled < kFrame)
                    continue;

                filled = 0;
                const auto e0 = nowMs();
                const int nbytes = opus_encode_float (encoder, pcm.data(), kFrame, opusBuf.data(), (opus_int32) opusBuf.size());
                encodeMsTotal = encodeMsTotal + (nowMs() - e0);
                ++framesEncoded;

                if (nbytes < 0)
                    continue;

                const auto gap = framePos > lastPacketEndPos ? framePos - lastPacketEndPos : lastPacketEndPos - framePos;
                const bool discontinuity = ! haveLast || gap > 600 || frameEpoch != lastEpochSent;
                lastEpochSent = frameEpoch;
                haveLast = true;
                lastPacketEndPos = framePos + (std::uint64_t) kFrame;

                packet.assign ((size_t) headerBytes + (size_t) nbytes, 0);
                packet[0] = 1;
                packet[1] = discontinuity ? 1 : 0;
                packet[4] = (std::uint8_t) frameEpoch;
                packet[5] = (std::uint8_t) (frameEpoch >> 8);
                std::memcpy (&packet[8], &seq, 4);
                std::memcpy (&packet[12], &framePos, 8);
                const std::uint64_t us = (std::uint64_t) (nowMs() * 1000.0);
                std::memcpy (&packet[20], &us, 8);
                std::memcpy (&packet[headerBytes], opusBuf.data(), (size_t) nbytes);
                ++seq;
                bytesSent += packet.size();

                PacketFn fn;
                {
                    std::lock_guard<std::mutex> lock (sinkMutex);
                    fn = sink;
                }

                if (fn)
                    fn (packet);
            }
        }
    }

    BrowserStream& owner;
    te::Engine& engine;
    ClockStats clock;
    BlockRing ring;
    juce::WaitableEvent pending;
    OpusEncoder* encoder = nullptr;
    std::thread encoderThread;
    std::atomic<bool> quit { false };
    std::mutex sourceMutex, sinkMutex;
    te::Edit* source = nullptr;
    PacketFn sink;
    std::atomic<std::int64_t> framesEncoded { 0 }, bytesSent { 0 }, ringOverflows { 0 };
    std::atomic<double> encodeMsTotal { 0 };
};

BrowserStream::BrowserStream (te::Engine& engine, int bitrateBps) : impl_ (std::make_unique<Impl> (*this, engine, bitrateBps))
{
    auto& dm = engine.getDeviceManager();
    dm.deviceManager.addAudioDeviceType (std::make_unique<StreamDeviceType> ([this] (const float* l, const float* r, int n) { impl_->onBlock (l, r, n); }, impl_->clock));
    dm.deviceManager.setCurrentAudioDeviceType ("BrowserStream", true);
    dm.initialise (0, 2);
    dm.rescanWaveDeviceList();

    // wave output devices of a custom device type appear asynchronously
    for (int i = 0; i < 60 && dm.getNumWaveOutDevices() == 0; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    ready_ = dm.getNumWaveOutDevices() > 0;
    impl_->encoderThread = std::thread ([this] { impl_->encoderLoop(); });
}

BrowserStream::~BrowserStream()
{
    impl_->engine.getDeviceManager().deviceManager.closeAudioDevice();      // stops the clock thread before the block sink goes away
    impl_.reset();
}

void BrowserStream::setPacketSink (PacketFn fn)
{
    std::lock_guard<std::mutex> lock (impl_->sinkMutex);
    impl_->sink = std::move (fn);
}

void BrowserStream::setPositionSource (te::Edit* e)
{
    std::lock_guard<std::mutex> lock (impl_->sourceMutex);
    impl_->source = e;
}

void BrowserStream::forget (te::Edit* e)
{
    std::lock_guard<std::mutex> lock (impl_->sourceMutex);

    if (impl_->source == e)
        impl_->source = nullptr;
}

json BrowserStream::stats() const
{
    const auto ticks = std::max<std::int64_t> (1, impl_->clock.ticks);
    return { { "frames_encoded", impl_->framesEncoded.load() },
             { "encode_ms_avg", impl_->encodeMsTotal.load() / (double) std::max<std::int64_t> (1, impl_->framesEncoded) },
             { "bytes_sent", impl_->bytesSent.load() },
             { "clock_ticks", impl_->clock.ticks.load() },
             { "clock_max_late_us", impl_->clock.maxLateUs.load() },
             { "clock_avg_late_us", (double) impl_->clock.lateUsTotal / (double) ticks },
             { "clock_late_gt2ms", impl_->clock.lateOver2ms.load() },
             { "clock_late_gt5ms", impl_->clock.lateOver5ms.load() },
             { "audio_callback_us_avg", (double) impl_->clock.callbackUsTotal / (double) ticks },
             { "audio_callback_us_max", impl_->clock.callbackUsMax.load() },
             { "ring_overflows", impl_->ringOverflows.load() },
             { "playing_projects", playingProjects_.load() },
             { "epoch", (int) epoch_.load() } };
}

} // namespace rovox
