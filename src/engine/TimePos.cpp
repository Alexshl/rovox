// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "TimePos.h"

#include <cmath>

namespace rovox
{

json timePosSchema()
{
    return { { "type", "object" }, { "description", "{s} | {bar, beat?, tick?} (1-based bar/beat, 960 ticks per quarter) | {qn} | {marker}" } };
}

json durationSchema()
{
    return { { "type", "object" }, { "description", "{s} | {bars} | {qn}" } };
}

static Error bad (const std::string& what, const std::string& path = {})
{
    return makeError (errc::invalidParams, what, path.empty() ? json (nullptr) : json { { "path", path } });
}

Result<double> timePosToSeconds (Project& p, const json& v)
{
    if (! v.is_object())
        return bad ("a time must be an object such as {\"s\": 12.5} or {\"bar\": 18, \"beat\": 1}");

    auto& edit = p.edit();

    if (v.contains ("s"))
    {
        if (! v["s"].is_number() || v["s"].get<double>() < 0.0)
            return bad ("s must be a number >= 0", "/s");

        return v["s"].get<double>();
    }

    if (v.contains ("qn"))
    {
        if (! v["qn"].is_number() || v["qn"].get<double>() < 0.0)
            return bad ("qn must be a number >= 0", "/qn");

        return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (v["qn"].get<double>())).inSeconds();
    }

    if (v.contains ("bar"))
    {
        if (! v["bar"].is_number_integer() || v["bar"].get<int>() < 1)
            return bad ("bar must be an integer >= 1", "/bar");

        const double beat = v.value ("beat", 1.0), tick = v.value ("tick", 0.0);

        if (beat < 1.0)
            return bad ("beat is 1-based", "/beat");

        te::tempo::BarsAndBeats bb;
        bb.bars = v["bar"].get<int>() - 1;
        bb.beats = te::BeatDuration::fromBeats ((beat - 1.0) + tick / 960.0);
        return edit.tempoSequence.toTime (bb).inSeconds();
    }

    if (v.contains ("marker"))
    {
        if (! v["marker"].is_string())
            return bad ("marker must be a marker id", "/marker");

        for (auto* m : edit.getMarkerManager().getMarkers())
            if (Project::idOf (m->state) == v["marker"].get<std::string>())
                return m->getPosition().getStart().inSeconds();

        return makeError (errc::notFound, "no marker " + v["marker"].get<std::string>(), json { { "marker_id", v["marker"] } });
    }

    return bad ("a time needs one of s, bar, qn or marker");
}

Result<double> durationToSeconds (Project& p, const json& v, double at)
{
    if (! v.is_object())
        return bad ("a duration must be an object such as {\"s\": 2} or {\"bars\": 2}");

    auto& ts = p.edit().tempoSequence;

    if (v.contains ("s"))
    {
        if (! v["s"].is_number() || v["s"].get<double>() < 0.0)
            return bad ("s must be a number >= 0", "/s");

        return v["s"].get<double>();
    }

    if (v.contains ("qn"))
    {
        if (! v["qn"].is_number() || v["qn"].get<double>() < 0.0)
            return bad ("qn must be a number >= 0", "/qn");

        const auto startBeats = ts.toBeats (te::TimePosition::fromSeconds (at));
        return ts.toTime (startBeats + te::BeatDuration::fromBeats (v["qn"].get<double>())).inSeconds() - at;
    }

    if (v.contains ("bars"))
    {
        if (! v["bars"].is_number() || v["bars"].get<double>() < 0.0)
            return bad ("bars must be a number >= 0", "/bars");

        const auto numerator = ts.getTimeSigAt (te::TimePosition::fromSeconds (at)).numerator.get();
        const auto startBeats = ts.toBeats (te::TimePosition::fromSeconds (at));
        return ts.toTime (startBeats + te::BeatDuration::fromBeats (v["bars"].get<double>() * numerator)).inSeconds() - at;
    }

    return bad ("a duration needs one of s, bars or qn");
}

Result<std::optional<double>> readTime (Project& p, const json& params, const std::string& key)
{
    if (params.contains (key + "_s"))
        return std::optional<double> (params[key + "_s"].get<double>());

    if (params.contains (key))
    {
        auto s = timePosToSeconds (p, params[key]);

        if (! s.ok())
            return s.error();

        return std::optional<double> (s.value());
    }

    return std::optional<double> {};
}

std::string barBeatString (te::Edit& edit, double seconds)
{
    const auto bb = edit.tempoSequence.toBarsAndBeats (te::TimePosition::fromSeconds (seconds));
    const double beats = bb.beats.inBeats();
    int whole = (int) std::floor (beats + 1.0e-9);
    int tick = (int) std::lround ((beats - whole) * 960.0);

    if (tick >= 960)
    {
        tick = 0;
        ++whole;
    }

    char buf[48];
    std::snprintf (buf, sizeof (buf), "%d.%d.%03d", bb.bars + 1, whole + 1, std::max (0, tick));
    return buf;
}

json describeTime (te::Edit& edit, double seconds)
{
    return { { "s", seconds }, { "bar_beat", barBeatString (edit, seconds) },
             { "qn", edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (seconds)).inBeats() } };
}

} // namespace rovox
