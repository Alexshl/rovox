// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Per-project transport (task 330) and meters (task 320). Both live on the message thread.
//
// Tracktion quirks handled here (spike P3, see ProjectTransport::apply / supervise):
//  - play() in the same message-loop turn as setPosition() is ignored: playback starts 40 ms after a seek;
//  - TransportControl::timerCallback() may stop a transport whose playhead has not started yet (about 1 start in 4):
//    a 30 ms supervisor re-issues play() until isPlaying() sticks.
#pragma once

#include "Project.h"

#include <chrono>
#include <functional>
#include <map>

namespace rovox
{

class ProjectTransport : private juce::Timer
{
public:
    using EventFn = std::function<void (const json&)>;

    ProjectTransport (Project&, EventFn);
    ~ProjectTransport() override;

    /** {state: playing|stopped, position_s, epoch, loop: {enabled, start_s, end_s}, length_s, audio}. */
    json state();

    /** params: state? ("playing"|"stopped"), position_s? / position? (TimePos), loop? {enabled?, start_s?, end_s?}.
        A position seeks (also while playing); state "playing" starts, "stopped" stops and keeps the position. */
    Result<json> apply (const json& params);

    bool wantsPlaying() const noexcept      { return wantPlaying_; }
    double positionSeconds();
    int supervisorRetries() const noexcept  { return retries_; }

private:
    void timerCallback() override;
    void startPlayback (bool afterSeek);
    void stopPlayback();
    void publish();

    Project& project_;
    EventFn event_;
    bool wantPlaying_ = false;
    int missedChecks_ = 0, retries_ = 0;
    bool playIssued_ = false;
    std::chrono::steady_clock::time_point playAt_;
};

/** Peak meters per audio track and the master. Reading is destructive on the Tracktion side (peak since the last read),
    so every reader keeps its own accumulator. */
class ProjectMeters
{
public:
    explicit ProjectMeters (Project&);
    ~ProjectMeters();

    struct Reading
    {
        std::string trackId;                // "master" for the master
        float peakDb[2] = { -100.0f, -100.0f };
    };

    /** Reads every tap once. The 30 Hz gateway timer calls this. */
    std::vector<Reading> sampleLatest();
    /** Peak since the previous call of this function (what the `mixer.get_meters` command returns). */
    std::vector<Reading> takePeakSinceLastCall();

private:
    struct Tap;
    void sync();
    void sample();

    Project& project_;
    std::vector<std::unique_ptr<Tap>> taps_;
};

} // namespace rovox
