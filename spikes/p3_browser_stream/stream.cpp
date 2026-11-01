// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Spike P3: browser-stream. A Tracktion Engine with a timer-driven "null clock" audio device renders in real time,
// the output is Opus-encoded (20 ms frames) and broadcast over a hand-written WebSocket server (also serves the
// static test page). Transport commands (play / stop / seek) arrive as JSON text frames.
//
//   rovox_spikes stream <work-dir> [--port 8765] [--www DIR] [--bitrate 160000] [--duration SECONDS]
//
// Wire format (little-endian binary frame, 28-byte header + Opus payload):
//   u8 type=1 | u8 flags (bit0 discontinuity) | u16 stream_id | u16 epoch | u16 reserved
//   u32 seq | u64 sample_pos (timeline, samples @48k) | u64 server_time_us | opus bytes
// Text frames (JSON): {"type":"transport","state":"playing|stopped","position_s":..,"epoch":N}, {"type":"stats",...}
// Client -> server: {"cmd":"play"|"stop"|"seek"|"stats","position_s":..}

#include "SpikeUtil.h"

#include <opus/opus.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
 #define MSG_NOSIGNAL 0
#endif

using namespace spike;
using namespace te::literals;

namespace
{
constexpr int kRate = 48000;
constexpr int kBlock = 480;                 // 10 ms device block
constexpr int kFrame = 960;                 // 20 ms Opus frame
constexpr size_t kMaxQueueBytes = 32 * 1024; // per-client back-pressure threshold (~1.3 s at 160 kbps)
std::atomic<bool> gStop { false };

// ---------------------------------------------------------------------------------------------
// Device: AudioIODevice driven by a high-priority thread on absolute steady_clock deadlines
// ---------------------------------------------------------------------------------------------
struct Block
{
    float l[kBlock], r[kBlock];
    uint64_t samplePos = 0;
    uint16_t epoch = 0;
    bool playing = false;
};

struct ClockStats
{
    std::atomic<int64_t> maxLateUs { 0 }, lateUsTotal { 0 };
    std::atomic<int64_t> ticks { 0 };
    std::atomic<int64_t> lateOver2ms { 0 }, lateOver5ms { 0 };
    std::atomic<int64_t> callbackUsTotal { 0 }, callbackUsMax { 0 };
};

class BrowserStreamDevice : public juce::AudioIODevice, private juce::Thread
{
public:
    using Sink = std::function<void (const float* l, const float* r, int n)>;

    BrowserStreamDevice (Sink s, ClockStats& st)
        : juce::AudioIODevice ("Browser Stream", "BrowserStream"), juce::Thread ("browser-stream clock"), sink (std::move (s)), stats (st) {}

    ~BrowserStreamDevice() override { stop(); }

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
            if (late > stats.maxLateUs) stats.maxLateUs = late;
            if (late > 2000) stats.lateOver2ms++;
            if (late > 5000) stats.lateOver5ms++;

            out.clear();
            float* const outs[2] = { out.getWritePointer (0), out.getWritePointer (1) };
            const auto c0 = clock::now();

            {
                const juce::ScopedLock sl (lock);

                if (callback != nullptr)
                    callback->audioDeviceIOCallbackWithContext (noInputs, 0, outs, 2, kBlock, {});
            }

            const auto cb = std::chrono::duration_cast<std::chrono::microseconds> (clock::now() - c0).count();
            stats.callbackUsTotal += cb;
            if (cb > stats.callbackUsMax) stats.callbackUsMax = cb;
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

class BrowserStreamDeviceType : public juce::AudioIODeviceType
{
public:
    BrowserStreamDeviceType (BrowserStreamDevice::Sink s, ClockStats& st) : juce::AudioIODeviceType ("BrowserStream"), sink (std::move (s)), stats (st) {}

    void scanForDevices() override                          {}
    juce::StringArray getDeviceNames (bool) const override  { return { "Browser Stream" }; }
    int getDefaultDeviceIndex (bool) const override         { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* d, bool) const override { return d != nullptr ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override       { return false; }

    juce::AudioIODevice* createDevice (const juce::String& out, const juce::String&) override
    {
        return out.isEmpty() || out == "Browser Stream" ? new BrowserStreamDevice (sink, stats) : nullptr;
    }

private:
    BrowserStreamDevice::Sink sink;
    ClockStats& stats;
};

// ---------------------------------------------------------------------------------------------
// Lock-free SPSC block ring
// ---------------------------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------------------------
// SHA-1 + minimal RFC 6455 server
// ---------------------------------------------------------------------------------------------
std::array<uint8_t, 20> sha1 (const std::string& s)
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::vector<uint8_t> m (s.begin(), s.end());
    const uint64_t bits = (uint64_t) s.size() * 8;
    m.push_back (0x80);

    while (m.size() % 64 != 56)
        m.push_back (0);

    for (int i = 7; i >= 0; --i)
        m.push_back ((uint8_t) (bits >> (i * 8)));

    auto rol = [] (uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

    for (size_t off = 0; off < m.size(); off += 64)
    {
        uint32_t w[80];

        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t) m[off + (size_t) i * 4] << 24 | (uint32_t) m[off + (size_t) i * 4 + 1] << 16
                 | (uint32_t) m[off + (size_t) i * 4 + 2] << 8 | (uint32_t) m[off + (size_t) i * 4 + 3];

        for (int i = 16; i < 80; ++i)
            w[i] = rol (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

            const uint32_t t = rol (a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol (b, 30); b = a; a = t;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    std::array<uint8_t, 20> out {};

    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            out[(size_t) i * 4 + (size_t) j] = (uint8_t) (h[i] >> (24 - 8 * j));

    return out;
}

std::string base64 (const void* data, size_t n)
{
    juce::MemoryOutputStream mo;
    juce::Base64::convertToBase64 (mo, data, n);
    return mo.toString().toStdString();
}

bool sendAll (int fd, const void* data, size_t n)
{
    auto* p = (const uint8_t*) data;

    while (n > 0)
    {
        const auto w = ::send (fd, p, n, MSG_NOSIGNAL);

        if (w <= 0)
            return false;

        p += w;
        n -= (size_t) w;
    }

    return true;
}

bool readExact (int fd, void* data, size_t n)
{
    auto* p = (uint8_t*) data;

    while (n > 0)
    {
        const auto r = ::recv (fd, p, n, 0);

        if (r <= 0)
            return false;

        p += r;
        n -= (size_t) r;
    }

    return true;
}

std::vector<uint8_t> makeFrame (uint8_t opcode, const uint8_t* payload, size_t n)
{
    std::vector<uint8_t> f;
    f.push_back ((uint8_t) (0x80 | opcode));

    if (n < 126)         f.push_back ((uint8_t) n);
    else if (n < 65536)  { f.push_back (126); f.push_back ((uint8_t) (n >> 8)); f.push_back ((uint8_t) n); }
    else                 { f.push_back (127); for (int i = 7; i >= 0; --i) f.push_back ((uint8_t) ((uint64_t) n >> (i * 8))); }

    f.insert (f.end(), payload, payload + n);
    return f;
}

struct Client
{
    int fd = -1;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<bool, std::vector<uint8_t>>> q;   // (isAudio, frame bytes)
    size_t queued = 0;
    bool needDiscontinuity = false;
    std::atomic<bool> alive { true };
    std::atomic<int64_t> droppedFrames { 0 };
    std::thread reader, writer;
};

class WsServer
{
public:
    std::function<void (const std::string&)> onCommand;
    std::function<std::string()> getIndexHtml;

    bool start (int port)
    {
        listenFd = ::socket (AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt (listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl (INADDR_ANY);
        a.sin_port = htons ((uint16_t) port);

        if (::bind (listenFd, (sockaddr*) &a, sizeof a) != 0 || ::listen (listenFd, 16) != 0)
            return false;

        acceptThread = std::thread ([this] { acceptLoop(); });
        return true;
    }

    void stop()
    {
        gStopping = true;
        ::shutdown (listenFd, SHUT_RDWR);
        ::close (listenFd);

        if (acceptThread.joinable())
            acceptThread.join();

        std::vector<std::shared_ptr<Client>> cs;
        {
            std::lock_guard<std::mutex> l (clientsMutex);
            cs = clients;
        }

        for (auto& c : cs)
            closeClient (*c);

        for (auto& c : cs)
        {
            if (c->reader.joinable()) c->reader.join();
            if (c->writer.joinable()) c->writer.join();
        }
    }

    void broadcastText (const std::string& text)
    {
        auto frame = makeFrame (0x1, (const uint8_t*) text.data(), text.size());
        std::lock_guard<std::mutex> l (clientsMutex);

        for (auto& c : clients)
        {
            if (! c->alive)
                continue;

            std::lock_guard<std::mutex> cl (c->m);
            c->queued += frame.size();
            c->q.emplace_back (false, frame);
            c->cv.notify_one();
        }
    }

    /** payload = full binary packet (header + opus). Flags byte is at payload[1]. */
    void broadcastAudio (std::vector<uint8_t>& payload)
    {
        std::lock_guard<std::mutex> l (clientsMutex);

        for (auto& c : clients)
        {
            if (! c->alive)
                continue;

            std::lock_guard<std::mutex> cl (c->m);

            // back-pressure: monitoring beats completeness, so drop the oldest audio and flag a discontinuity
            while (c->queued > kMaxQueueBytes)
            {
                auto it = std::find_if (c->q.begin(), c->q.end(), [] (auto& e) { return e.first; });

                if (it == c->q.end())
                    break;

                c->queued -= it->second.size();
                c->q.erase (it);
                c->droppedFrames++;
                c->needDiscontinuity = true;
            }

            auto copy = payload;

            if (c->needDiscontinuity)
            {
                copy[1] |= 1;
                c->needDiscontinuity = false;
            }

            auto frame = makeFrame (0x2, copy.data(), copy.size());
            c->queued += frame.size();
            c->q.emplace_back (true, std::move (frame));
            c->cv.notify_one();
        }
    }

    int numClients()
    {
        std::lock_guard<std::mutex> l (clientsMutex);
        return (int) clients.size();
    }

    int64_t totalDropped()
    {
        std::lock_guard<std::mutex> l (clientsMutex);
        int64_t n = 0;

        for (auto& c : clients)
            n += c->droppedFrames;

        return n;
    }

private:
    static void closeClient (Client& c)
    {
        c.alive = false;
        ::shutdown (c.fd, SHUT_RDWR);
        std::lock_guard<std::mutex> l (c.m);
        c.cv.notify_all();
    }

    void acceptLoop()
    {
        while (! gStopping)
        {
            const int fd = ::accept (listenFd, nullptr, nullptr);

            if (fd < 0)
                break;

            int one = 1;
            setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            int sndbuf = 16 * 1024;     // small kernel buffer so a stalled client shows up as back-pressure in our own queue
            setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
            std::thread ([this, fd] { handshakeAndServe (fd); }).detach();
        }
    }

    void handshakeAndServe (int fd)
    {
        std::string req;
        char ch;

        while (req.size() < 8192 && req.find ("\r\n\r\n") == std::string::npos)
        {
            if (::recv (fd, &ch, 1, 0) <= 0) { ::close (fd); return; }
            req.push_back (ch);
        }

        auto headerValue = [&req] (const std::string& name) -> std::string
        {
            auto lower = req;
            std::transform (lower.begin(), lower.end(), lower.begin(), ::tolower);
            auto pos = lower.find ("\r\n" + name + ":");

            if (pos == std::string::npos)
                return {};

            pos += name.size() + 3;
            auto end = req.find ("\r\n", pos);
            auto v = req.substr (pos, end - pos);
            v.erase (0, v.find_first_not_of (' '));
            return v;
        };

        const auto key = headerValue ("sec-websocket-key");

        if (key.empty())
        {
            const auto body = getIndexHtml ? getIndexHtml() : std::string ("rovox browser-stream spike");
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string (body.size())
                                     + "\r\nCross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\nConnection: close\r\n\r\n";
            sendAll (fd, head.data(), head.size());
            sendAll (fd, body.data(), body.size());
            ::close (fd);
            return;
        }

        const auto digest = sha1 (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                                 + base64 (digest.data(), digest.size()) + "\r\n\r\n";

        if (! sendAll (fd, resp.data(), resp.size()))
        {
            ::close (fd);
            return;
        }

        auto c = std::make_shared<Client>();
        c->fd = fd;
        c->writer = std::thread ([this, c] { writerLoop (*c); });
        c->reader = std::thread ([this, c] { readerLoop (*c); });
        std::lock_guard<std::mutex> l (clientsMutex);
        clients.push_back (c);
    }

    void writerLoop (Client& c)
    {
        while (c.alive)
        {
            std::vector<uint8_t> frame;
            {
                std::unique_lock<std::mutex> l (c.m);
                c.cv.wait (l, [&c] { return ! c.q.empty() || ! c.alive; });

                if (! c.alive)
                    break;

                frame = std::move (c.q.front().second);
                c.queued -= frame.size();
                c.q.pop_front();
            }

            if (! sendAll (c.fd, frame.data(), frame.size()))
                break;
        }

        c.alive = false;
    }

    void readerLoop (Client& c)
    {
        while (c.alive)
        {
            uint8_t h[2];

            if (! readExact (c.fd, h, 2))
                break;

            const uint8_t opcode = h[0] & 0xF;
            uint64_t len = h[1] & 0x7F;

            if (len == 126)      { uint8_t e[2]; if (! readExact (c.fd, e, 2)) break; len = (uint64_t) e[0] << 8 | e[1]; }
            else if (len == 127) { uint8_t e[8]; if (! readExact (c.fd, e, 8)) break; len = 0; for (auto b : e) len = len << 8 | b; }

            uint8_t mask[4] = {};

            if ((h[1] & 0x80) && ! readExact (c.fd, mask, 4))
                break;

            if (len > (1u << 20))
                break;

            std::vector<uint8_t> payload ((size_t) len);

            if (len > 0 && ! readExact (c.fd, payload.data(), (size_t) len))
                break;

            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] ^= mask[i % 4];

            if (opcode == 0x8)
                break;

            if (opcode == 0x9)
            {
                auto pong = makeFrame (0xA, payload.data(), payload.size());
                std::lock_guard<std::mutex> l (c.m);
                c.q.emplace_back (false, pong);
                c.queued += pong.size();
                c.cv.notify_one();
            }
            else if (opcode == 0x1 && onCommand)
                onCommand (std::string (payload.begin(), payload.end()));
        }

        closeClient (c);   // dead clients stay in the vector until stop() (spike: no reaping)
    }

    int listenFd = -1;
    std::atomic<bool> gStopping { false };
    std::thread acceptThread;
    std::mutex clientsMutex;
    std::vector<std::shared_ptr<Client>> clients;
};

// ---------------------------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------------------------
struct StreamApp
{
    StreamApp (const juce::File& dir, int port, int bitrate, const juce::File& wwwDir)
        : workDir (dir), www (wwwDir), bitrate (bitrate)
    {
        auto behaviour = std::make_unique<SpikeBehaviour>();
        behaviour->systemDevices = false;
        behaviour->autoInit = false;
        behaviour->useCgroup = true;
        engine = std::make_unique<te::Engine> ("rovox_p3_stream", nullptr, std::move (behaviour));

        auto& dm = engine->getDeviceManager();
        dm.deviceManager.addAudioDeviceType (std::make_unique<BrowserStreamDeviceType> ([this] (const float* l, const float* r, int n) { onBlock (l, r, n); }, clock));
        dm.deviceManager.setCurrentAudioDeviceType ("BrowserStream", true);
        dm.initialise (0, 2);

        // wave output devices are built asynchronously from the device's channel list: wait for them, otherwise
        // the Edit's playback context has nowhere to send audio
        dm.rescanWaveDeviceList();

        for (int i = 0; i < 40 && dm.getNumWaveOutDevices() == 0; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        auto* dev = dm.deviceManager.getCurrentAudioDevice();
        std::printf ("device: %s, %.0f Hz, block %d\n", dev != nullptr ? dev->getName().toRawUTF8() : "NONE",
                     dev != nullptr ? dev->getCurrentSampleRate() : 0.0, dev != nullptr ? dev->getCurrentBufferSizeSamples() : 0);
        std::printf ("wave outputs: %d, default out: %s\n", dm.getNumWaveOutDevices(),
                     dm.getDefaultWaveOutDevice() != nullptr ? dm.getDefaultWaveOutDevice()->getName().toRawUTF8() : "none");

        buildEdit();

        int err = 0;
        encoder = opus_encoder_create (kRate, 2, OPUS_APPLICATION_AUDIO, &err);
        opus_encoder_ctl (encoder, OPUS_SET_BITRATE (bitrate));
        opus_encoder_ctl (encoder, OPUS_SET_COMPLEXITY (5));

        server.getIndexHtml = [this] { return www.getChildFile ("index.html").loadFileAsString().toStdString(); };
        server.onCommand = [this] (const std::string& s) { juce::MessageManager::callAsync ([this, s] { handleCommand (s); }); };

        if (! server.start (port))
            std::printf ("cannot listen on port %d\n", port);
        else
            std::printf ("listening on :%d (page: http://localhost:%d/)\n", port, port);

        encoderThread = std::thread ([this] { encoderLoop(); });

        supervisor.fn = [this] { supervise(); };
        supervisor.startTimer (30);
    }

    /** Finding: TransportControl::timerCallback() stops the transport when it is "playing" but the playhead has not
        started yet (the play command is applied by the audio thread on a later block). When that timer wins the race
        play() is silently undone, roughly 1 in 4 starts in this spike. Re-issue play() until the playhead runs. */
    void supervise()
    {
        if (! wantPlaying || edit == nullptr)
            return;

        auto& tc = edit->getTransport();

        if (tc.isPlaying())
        {
            missedChecks = 0;
        }
        else if (++missedChecks >= 2)
        {
            tc.play (false);
            ++playRetries;
            missedChecks = 0;
        }
    }

    ~StreamApp()
    {
        supervisor.stopTimer();
        editForClock = nullptr;
        quit = true;
        pending.signal();

        if (encoderThread.joinable())
            encoderThread.join();

        server.stop();
        edit.reset();
        opus_encoder_destroy (encoder);
    }

    void buildEdit()
    {
        const int n = kRate * 120;
        std::vector<float> click ((size_t) n), tone ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            click[(size_t) i] = (i % (kRate / 2)) < 96 ? 0.6f : 0.0f;   // 2 ms click every 0.5 s on the timeline
            tone[(size_t) i] = 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * i / kRate);
        }

        const auto clickFile = workDir.getChildFile ("click.wav"), toneFile = workDir.getChildFile ("tone.wav");
        writeWav (clickFile, { click }, kRate);
        writeWav (toneFile, { tone }, kRate);

        edit = te::Edit::createSingleTrackEdit (*engine, te::Edit::EditRole::forEditing);
        edit->ensureNumberOfAudioTracks (2);
        auto tracks = te::getAudioTracks (*edit);
        te::insertWaveClip (*tracks[0], {}, clickFile, { .time = { 0_tp, 120_td } }, te::DeleteExistingClips::no);
        te::insertWaveClip (*tracks[1], {}, toneFile, { .time = { 0_tp, 120_td } }, te::DeleteExistingClips::no);
        edit->getTransport().ensureContextAllocated();
        editForClock = edit.get();
        std::printf ("playback context: %s\n", edit->getTransport().getCurrentPlaybackContext() != nullptr ? "allocated" : "NONE");
    }

    // device clock thread
    void onBlock (const float* l, const float* r, int n)
    {
        Block b;
        std::memcpy (b.l, l, sizeof (float) * (size_t) n);
        std::memcpy (b.r, r, sizeof (float) * (size_t) n);
        {
            double ss = 0;

            for (int i = 0; i < n; ++i)
                ss += (double) l[i] * l[i];

            lastBlockRmsDb = 10.0 * std::log10 (std::max (ss / n, 1.0e-20));
        }

        b.playing = streamPlaying.load();
        b.epoch = epoch.load();

        // the device starts inside DeviceManager::initialise(), before the Edit exists
        auto* e = editForClock.load();

        if (e != nullptr)
            if (auto* ctx = e->getTransport().getCurrentPlaybackContext())
                b.samplePos = (uint64_t) std::max (0.0, ctx->getPosition().inSeconds() * kRate);

        if (! ring.push (b))
            ringOverflows++;

        pending.signal();
    }

    // encoder thread
    void encoderLoop()
    {
        std::array<float, kFrame * 2> pcm {};
        int filled = 0;
        uint16_t frameEpoch = 0;
        uint64_t framePos = 0, lastPacketEndPos = 0;
        bool haveLast = false;
        uint32_t seq = 0;
        std::vector<uint8_t> opusBuf (1500);

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

                const bool discontinuity = ! haveLast || (framePos > lastPacketEndPos ? framePos - lastPacketEndPos : lastPacketEndPos - framePos) > 600
                                           || frameEpoch != lastEpochSent;
                lastEpochSent = frameEpoch;
                haveLast = true;
                lastPacketEndPos = framePos + (uint64_t) kFrame;

                std::vector<uint8_t> pkt (28 + (size_t) nbytes);
                pkt[0] = 1;
                pkt[1] = discontinuity ? 1 : 0;
                pkt[2] = 0; pkt[3] = 0;                          // stream_id 0
                pkt[4] = (uint8_t) frameEpoch; pkt[5] = (uint8_t) (frameEpoch >> 8);
                pkt[6] = 0; pkt[7] = 0;
                std::memcpy (&pkt[8], &seq, 4);
                std::memcpy (&pkt[12], &framePos, 8);
                const uint64_t us = (uint64_t) (nowMs() * 1000.0);
                std::memcpy (&pkt[20], &us, 8);
                std::memcpy (&pkt[28], opusBuf.data(), (size_t) nbytes);
                ++seq;
                bytesSent += pkt.size();
                server.broadcastAudio (pkt);
            }
        }
    }

    // message thread
    void handleCommand (const std::string& json)
    {
        const auto v = juce::JSON::parse (juce::String (json));
        const auto cmd = v["cmd"].toString();
        auto& tc = edit->getTransport();
        const bool hasPos = v.hasProperty ("position_s");

        if (cmd == "play")
        {
            // Finding: TransportControl::play() issued in the same message-loop iteration as setPosition() is
            // ignored (the transport state machine is driven by the position change first), so start playback one
            // loop turn later.
            if (hasPos)
                tc.setPosition (te::TimePosition::fromSeconds ((double) v["position_s"]));

            juce::Timer::callAfterDelay (hasPos ? 40 : 0, [this]
            {
                edit->getTransport().play (false);
                wantPlaying = true;
                streamPlaying = true;
                cpuStartUs = cpuUs();
                wallStartMs = nowMs();
                ++epoch;
                sendTransport ("playing");
            });
        }
        else if (cmd == "stop")
        {
            tc.stop (false, false);
            wantPlaying = false;
            streamPlaying = false;
            ++epoch;
            sendTransport ("stopped");
        }
        else if (cmd == "seek")
        {
            tc.setPosition (te::TimePosition::fromSeconds ((double) v["position_s"]));
            ++epoch;
            sendTransport (streamPlaying ? "playing" : "stopped");
        }
        else if (cmd == "stats")
            server.broadcastText (statsJson());
    }

    void sendTransport (const char* state)
    {
        server.broadcastText (juce::JSON::toString (makeObject ({ { "type", "transport" }, { "state", state },
                                                                  { "position_s", edit->getTransport().getPosition().inSeconds() },
                                                                  { "epoch", (int) epoch.load() },
                                                                  { "server_time_us", (juce::int64) (nowMs() * 1000.0) } }), true).toStdString());
    }

    static double cpuUs()
    {
        rusage ru {};
        getrusage (RUSAGE_SELF, &ru);
        return ru.ru_utime.tv_sec * 1.0e6 + ru.ru_utime.tv_usec + ru.ru_stime.tv_sec * 1.0e6 + ru.ru_stime.tv_usec;
    }

    std::string statsJson()
    {
        const auto wall = nowMs() - wallStartMs;
        const auto cpu = cpuUs() - cpuStartUs;
        const auto ticks = std::max<int64_t> (1, clock.ticks);
        return juce::JSON::toString (makeObject ({
            { "type", "stats" },
            { "clients", server.numClients() },
            { "frames_encoded", (juce::int64) framesEncoded.load() },
            { "encode_ms_avg", encodeMsTotal.load() / std::max<int64_t> (1, framesEncoded) },
            { "bytes_sent", (juce::int64) bytesSent.load() },
            { "kbps_avg", bytesSent * 8.0 / std::max (1.0, wall) },
            { "clock_ticks", (juce::int64) clock.ticks.load() },
            { "clock_max_late_us", (juce::int64) clock.maxLateUs.load() },
            { "clock_avg_late_us", (double) clock.lateUsTotal / ticks },
            { "clock_late_gt2ms", (juce::int64) clock.lateOver2ms.load() },
            { "clock_late_gt5ms", (juce::int64) clock.lateOver5ms.load() },
            { "audio_callback_us_avg", (double) clock.callbackUsTotal / ticks },
            { "audio_callback_us_max", (juce::int64) clock.callbackUsMax.load() },
            { "ring_overflows", (juce::int64) ringOverflows.load() },
            { "dropped_audio_frames", (juce::int64) server.totalDropped() },
            { "process_cpu_percent_of_one_core_since_play", streamPlaying ? 100.0 * cpu / (wall * 1000.0) : -1.0 },
            { "engine_cpus_for_audio", cpusAvailableToUs() },
            { "transport_play_retries_by_supervisor", playRetries.load() },
            { "debug_transport_is_playing", edit->getTransport().isPlaying() },
            { "debug_transport_position_s", edit->getTransport().getPosition().inSeconds() },
            { "debug_last_block_rms_db", lastBlockRmsDb.load() },
            { "debug_ctx_is_playing", edit->getTransport().getCurrentPlaybackContext() != nullptr && edit->getTransport().getCurrentPlaybackContext()->isPlaying() },
            { "debug_ctx_position_s", edit->getTransport().getCurrentPlaybackContext() != nullptr ? edit->getTransport().getCurrentPlaybackContext()->getPosition().inSeconds() : -1.0 },
            { "debug_ctx_unlooped_position_s", edit->getTransport().getCurrentPlaybackContext() != nullptr ? edit->getTransport().getCurrentPlaybackContext()->getUnloopedPosition().inSeconds() : -1.0 },
            { "debug_ctx_audible_s", edit->getTransport().getCurrentPlaybackContext() != nullptr ? edit->getTransport().getCurrentPlaybackContext()->getAudibleTimelineTime().inSeconds() : -1.0 },
            { "debug_ctx_latency_samples", edit->getTransport().getCurrentPlaybackContext() != nullptr ? edit->getTransport().getCurrentPlaybackContext()->getLatencySamples() : -1 },
            { "peak_rss_mb", peakRssKb() / 1024.0 } }), true).toStdString();
    }

    juce::File workDir, www;
    int bitrate;
    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<te::Edit> edit;
    std::atomic<te::Edit*> editForClock { nullptr };
    ClockStats clock;
    BlockRing ring;
    juce::WaitableEvent pending;
    OpusEncoder* encoder = nullptr;
    WsServer server;
    std::thread encoderThread;
    std::atomic<bool> quit { false }, streamPlaying { false }, wantPlaying { false };
    int missedChecks = 0;
    std::atomic<int> playRetries { 0 };
    struct Supervisor : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { if (fn) fn(); }
    } supervisor;
    std::atomic<uint16_t> epoch { 0 };
    uint16_t lastEpochSent = 0xffff;
    std::atomic<int64_t> framesEncoded { 0 }, bytesSent { 0 }, ringOverflows { 0 };
    std::atomic<double> encodeMsTotal { 0 }, lastBlockRmsDb { -200 };
    double cpuStartUs = 0, wallStartMs = 0;
};

void onSignal (int) { gStop = true; }
}

int runStream (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: rovox_spikes stream <work-dir> [--port N] [--www DIR] [--bitrate N] [--duration S]\n");
        return 2;
    }

    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    dir.createDirectory();
    int port = 8765, bitrate = 160000;
    double duration = 0;
    juce::File www = juce::File::getCurrentWorkingDirectory().getChildFile ("spikes/p3_browser_stream/www");

    for (int i = 2; i + 1 < argc; i += 2)
    {
        const juce::String a (argv[i]), v (argv[i + 1]);
        if (a == "--port")          port = v.getIntValue();
        else if (a == "--www")      www = juce::File::getCurrentWorkingDirectory().getChildFile (v);
        else if (a == "--bitrate")  bitrate = v.getIntValue();
        else if (a == "--duration") duration = v.getDoubleValue();
    }

    std::signal (SIGTERM, onSignal);
    std::signal (SIGINT, onSignal);
    std::signal (SIGPIPE, SIG_IGN);

    {
        StreamApp app (dir, port, bitrate, www);
        const auto t0 = nowMs();

        while (! gStop && (duration <= 0 || nowMs() - t0 < duration * 1000.0))
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        std::printf ("final stats: %s\n", app.statsJson().c_str());
    }

    return 0;
}
