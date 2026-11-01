// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Transport.h"

#include "TimePos.h"

#include <cmath>

namespace rovox
{

ProjectTransport::ProjectTransport (Project& p, EventFn e) : project_ (p), event_ (std::move (e)) {}

ProjectTransport::~ProjectTransport()
{
    stopTimer();

    if (wantPlaying_)
    {
        project_.edit().getTransport().stop (false, false);

        if (auto* s = project_.host().stream())
        {
            s->addPlayingProjects (-1);
            s->forget (&project_.edit());
        }
    }
}

double ProjectTransport::positionSeconds()
{
    auto& tc = project_.edit().getTransport();

    if (wantPlaying_ && tc.isPlaying())
        if (auto* ctx = tc.getCurrentPlaybackContext())
            return ctx->getPosition().inSeconds();

    return tc.getPosition().inSeconds();
}

json ProjectTransport::state()
{
    auto& edit = project_.edit();
    auto& tc = edit.getTransport();
    const auto range = tc.getLoopRange();
    auto* stream = project_.host().stream();

    return { { "state", wantPlaying_ ? "playing" : "stopped" },
             { "position_s", positionSeconds() },
             { "epoch", stream != nullptr ? (int) stream->epoch() : 0 },
             { "loop", { { "enabled", tc.looping.get() }, { "start_s", range.getStart().inSeconds() }, { "end_s", range.getEnd().inSeconds() } } },
             { "length_s", edit.getLength().inSeconds() },
             { "audio", toString (project_.host().audioMode()) } };
}

void ProjectTransport::publish()
{
    if (event_)
    {
        auto s = state();
        s["type"] = "transport.changed";
        s["project_id"] = project_.id();
        event_ (s);
    }
}

Result<json> ProjectTransport::apply (const json& params)
{
    auto& edit = project_.edit();
    auto& tc = edit.getTransport();
    auto* stream = project_.host().stream();

    const std::string wanted = params.value ("state", "");

    if (! wanted.empty() && wanted != "playing" && wanted != "stopped")
        return makeError (errc::invalidParams, "state must be \"playing\" or \"stopped\"", json { { "path", "/state" } });

    auto position = readTime (project_, params, "position");

    if (! position.ok())
        return position.error();

    if (wanted == "playing" && stream == nullptr)
        return makeError (errc::preconditionFailed, "there is no audio output: start the server with --audio browser to play", json { { "audio", toString (project_.host().audioMode()) } });

    if (wanted == "playing" && ! stream->ready())
        return makeError (errc::preconditionFailed, "the browser-stream audio device did not come up");

    if (params.contains ("loop"))
    {
        const auto& l = params["loop"];
        auto range = tc.getLoopRange();
        double start = range.getStart().inSeconds(), end = range.getEnd().inSeconds();

        if (l.contains ("start_s"))
            start = l["start_s"].get<double>();

        if (l.contains ("end_s"))
            end = l["end_s"].get<double>();

        if (end <= start && (l.value ("enabled", tc.looping.get())))
            return makeError (errc::invalidParams, "loop end must be after loop start", json { { "path", "/loop" } });

        if (end > start)
            tc.setLoopRange (te::TimeRange (te::TimePosition::fromSeconds (start), te::TimePosition::fromSeconds (end)));

        if (l.contains ("enabled"))
            tc.looping = l["enabled"].get<bool>();
    }

    const bool seek = position.value().has_value();

    if (seek)
    {
        tc.setPosition (te::TimePosition::fromSeconds (std::max (0.0, *position.value())));

        if (stream != nullptr)
            stream->bumpEpoch();
    }

    if (wanted == "playing" && ! wantPlaying_)
        startPlayback (seek);
    else if (wanted == "stopped" && wantPlaying_)
        stopPlayback();

    publish();
    return state();
}

void ProjectTransport::startPlayback (bool afterSeek)
{
    auto& edit = project_.edit();
    auto& tc = edit.getTransport();
    auto* stream = project_.host().stream();

    tc.ensureContextAllocated();
    project_.meters().sampleLatest();       // attach the level taps before the first block plays
    wantPlaying_ = true;
    missedChecks_ = 0;
    stream->setPositionSource (&edit);
    stream->addPlayingProjects (1);
    stream->bumpEpoch();

    // play() in the same message-loop turn as setPosition() is ignored; the supervisor issues the first play() 40 ms later
    playAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds (afterSeek ? 40 : 0);
    playIssued_ = false;
    startTimer (30);
}

void ProjectTransport::stopPlayback()
{
    auto& tc = project_.edit().getTransport();
    wantPlaying_ = false;
    stopTimer();
    tc.stop (false, false);

    if (auto* s = project_.host().stream())
    {
        s->addPlayingProjects (-1);
        s->bumpEpoch();
    }
}

/** TransportControl::timerCallback() stops a transport that is "playing" while the playhead has not started yet (the
    play command is applied by the audio thread on a later block). When that timer wins the race play() is silently
    undone; re-issue it until the playhead runs. */
void ProjectTransport::timerCallback()
{
    if (! wantPlaying_)
        return;

    auto& tc = project_.edit().getTransport();

    if (! playIssued_)
    {
        if (std::chrono::steady_clock::now() >= playAt_)
        {
            tc.play (false);
            playIssued_ = true;
        }

        return;
    }

    if (tc.isPlaying())
    {
        missedChecks_ = 0;
    }
    else if (++missedChecks_ >= 2)
    {
        tc.play (false);
        ++retries_;
        missedChecks_ = 0;
    }
}

//==============================================================================
struct ProjectMeters::Tap
{
    Tap (std::string id, juce::WeakReference<te::LevelMeasurer> m) : trackId (std::move (id)), measurer (m)
    {
        if (measurer != nullptr)
            measurer->addClient (client);
    }

    ~Tap()
    {
        if (measurer != nullptr)
            measurer->removeClient (client);
    }

    std::string trackId;
    juce::WeakReference<te::LevelMeasurer> measurer;
    te::LevelMeasurer::Client client;
    float latest[2] = { -100.0f, -100.0f };
    float sinceCommand[2] = { -100.0f, -100.0f };
};

ProjectMeters::ProjectMeters (Project& p) : project_ (p) {}
ProjectMeters::~ProjectMeters() = default;

void ProjectMeters::sync()
{
    auto& edit = project_.edit();
    std::vector<std::unique_ptr<Tap>> next;

    auto find = [this] (const std::string& id, te::LevelMeasurer* m) -> std::unique_ptr<Tap>
    {
        for (auto& t : taps_)
            if (t != nullptr && t->trackId == id && t->measurer.get() == m)
                return std::move (t);

        return nullptr;
    };

    for (auto* t : te::getAudioTracks (edit))
    {
        auto* meter = t->getLevelMeterPlugin();
        const auto id = Project::idOf (t->state);
        te::LevelMeasurer* m = meter != nullptr ? &meter->measurer : nullptr;
        auto tap = find (id, m);
        next.push_back (tap != nullptr ? std::move (tap) : std::make_unique<Tap> (id, juce::WeakReference<te::LevelMeasurer> (m)));
    }

    te::LevelMeasurer* master = nullptr;

    if (auto* ctx = edit.getTransport().getCurrentPlaybackContext())
        master = &ctx->masterLevels;

    auto tap = find ("master", master);
    next.push_back (tap != nullptr ? std::move (tap) : std::make_unique<Tap> ("master", juce::WeakReference<te::LevelMeasurer> (master)));
    taps_ = std::move (next);          // stale taps are destroyed here (their clients are removed while the measurer still lives)
}

void ProjectMeters::sample()
{
    sync();

    for (auto& t : taps_)
    {
        const int used = t->client.getNumChannelsUsed();
        float db[2];
        db[0] = t->client.getAndClearAudioLevel (0).dB;
        db[1] = used > 1 ? t->client.getAndClearAudioLevel (1).dB : db[0];

        for (int c = 0; c < 2; ++c)
        {
            t->latest[c] = db[c];
            t->sinceCommand[c] = std::max (t->sinceCommand[c], db[c]);
        }
    }
}

std::vector<ProjectMeters::Reading> ProjectMeters::sampleLatest()
{
    sample();
    std::vector<Reading> out;

    for (auto& t : taps_)
    {
        Reading r;
        r.trackId = t->trackId;
        r.peakDb[0] = t->latest[0];
        r.peakDb[1] = t->latest[1];
        out.push_back (r);
    }

    return out;
}

std::vector<ProjectMeters::Reading> ProjectMeters::takePeakSinceLastCall()
{
    sample();
    std::vector<Reading> out;

    for (auto& t : taps_)
    {
        Reading r;
        r.trackId = t->trackId;
        r.peakDb[0] = t->sinceCommand[0];
        r.peakDb[1] = t->sinceCommand[1];
        t->sinceCommand[0] = t->sinceCommand[1] = -100.0f;
        out.push_back (r);
    }

    return out;
}

} // namespace rovox
