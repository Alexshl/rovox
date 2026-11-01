// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Browser stream (task 330): a juce::AudioIODevice driven by a steady_clock thread (no sound card), whose output is
// Opus-encoded (20 ms frames, 48 kHz stereo) and handed to a packet sink (the WebSocket gateway). One mix per process:
// every Edit that plays is summed by Tracktion's DeviceManager before it reaches this device.
//
// Packet (little endian, 28-byte header + Opus payload):
//   u8 type=1 | u8 flags (bit0 discontinuity) | u16 stream_id | u16 epoch | u16 reserved
//   u32 seq | u64 sample_pos (timeline of the position source, samples @48k) | u64 server_time_us | opus bytes
#pragma once

#include "Tracktion.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

struct OpusEncoder;

namespace rovox
{

class BrowserStream
{
public:
    static constexpr int sampleRate = 48000;
    static constexpr int blockSize = 480;          // 10 ms device block
    static constexpr int frameSize = 960;          // 20 ms Opus frame
    static constexpr int headerBytes = 28;

    using PacketFn = std::function<void (const std::vector<std::uint8_t>& packet)>;

    /** Message thread. Registers the device type, opens the device and waits (pumping the message loop) until Tracktion
        has built its wave output devices; an Edit created before that would play into nothing. */
    explicit BrowserStream (te::Engine&, int bitrateBps = 160000);
    ~BrowserStream();

    bool ready() const noexcept             { return ready_; }

    /** Called on the encoder thread for every packet. Must not block. */
    void setPacketSink (PacketFn);

    /** Message thread. The Edit whose playhead stamps `sample_pos`; call forget() before destroying an Edit. */
    void setPositionSource (te::Edit*);
    void forget (te::Edit*);

    void addPlayingProjects (int delta)     { playingProjects_ += delta; }
    int playingProjects() const noexcept    { return playingProjects_.load(); }

    /** Any transport change (play, stop, seek) starts a new epoch; packets of older epochs are stale. */
    std::uint16_t bumpEpoch()               { return ++epoch_; }
    std::uint16_t epoch() const noexcept    { return epoch_.load(); }

    json stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> ready_ { false };
    std::atomic<int> playingProjects_ { 0 };
    std::atomic<std::uint16_t> epoch_ { 0 };
};

} // namespace rovox
