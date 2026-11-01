// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Commands added in M2 for the MCP tool set (task 210): project.get, items, tempo, time.convert, marker.upsert, fx.list /
// fx.params / fx.update. Same rules as every command: ids not indexes, explicit units, one call = one undo step.
#include "Command.h"

#include "TimePos.h"

#include <cmath>

namespace rovox
{

namespace
{
json obj (std::initializer_list<std::pair<const char*, json>> props, std::initializer_list<const char*> required = {})
{
    json properties = json::object();

    for (auto& [k, v] : props)
        properties[k] = v;

    return { { "type", "object" }, { "properties", properties }, { "required", json (std::vector<std::string> (required.begin(), required.end())) },
             { "additionalProperties", false } };
}

json str()                          { return { { "type", "string" } }; }
json strMin (int n)                 { return { { "type", "string" }, { "minLength", n } }; }
json num()                          { return { { "type", "number" } }; }
json numMin (double lo)             { return { { "type", "number" }, { "minimum", lo } }; }
json boolean()                      { return { { "type", "boolean" } }; }
json anyObject()                    { return { { "type", "object" } }; }
json arrayOf (json items)           { return { { "type", "array" }, { "items", std::move (items) } }; }

std::string colourHex (juce::Colour c)  { return "#" + c.toDisplayString (false).toLowerCase().toStdString(); }

te::Clip* findClipById (Project& p, const std::string& id)
{
    return p.findClip (id);
}

json clipJson (Project& p, te::Clip& c)
{
    const double start = c.getPosition().getStart().inSeconds(), len = c.getPosition().getLength().inSeconds();
    json j { { "item_id", Project::idOf (c.state) }, { "name", toStd (c.getName()) }, { "start_s", start }, { "length_s", len }, { "end_s", start + len },
             { "start_bar_beat", barBeatString (p.edit(), start) }, { "muted", c.isMuted() } };

    if (auto* t = c.getClipTrack())
        j["track_id"] = Project::idOf (t->state);

    j["offset_s"] = c.getPosition().getOffset().inSeconds();

    if (auto* a = dynamic_cast<te::AudioClipBase*> (&c))
    {
        j["file"] = toStd (a->getOriginalFile().getFileName());
        j["fade_in_s"] = a->getFadeIn().inSeconds();
        j["fade_out_s"] = a->getFadeOut().inSeconds();
        j["gain_db"] = (double) a->getGainDB();
    }

    return j;
}

json paramJson (te::AutomatableParameter& ap)
{
    const auto range = ap.getValueRange();
    return { { "ident", toStd (ap.paramID) }, { "name", toStd (ap.getParameterName()) }, { "value", (double) ap.getCurrentExplicitValue() },
             { "display", toStd (ap.valueToString (ap.getCurrentExplicitValue())) }, { "label", toStd (ap.getLabel()) },
             { "min", (double) range.getStart() }, { "max", (double) range.getEnd() } };
}

json pluginInfo (te::Plugin& p, bool withParams)
{
    json j { { "plugin_id", Project::idOf (p.state) }, { "name", toStd (p.getName()) }, { "type", toStd (p.getPluginType()) }, { "enabled", p.isEnabled() } };

    if (auto* ext = dynamic_cast<te::ExternalPlugin*> (&p))
    {
        j["loaded"] = ext->getAudioPluginInstance() != nullptr;

        if (ext->getAudioPluginInstance() == nullptr)
            j["load_error"] = toStd (ext->getLoadError());
    }

    if (withParams)
    {
        json params = json::array();

        for (auto* ap : p.getAutomatableParameters())
            params.push_back (paramJson (*ap));

        j["params"] = params;
    }

    return j;
}

/** Plugin list of a track id or "master". Hides nothing: the volume fader is part of the chain, flagged so agents leave it alone. */
Result<te::PluginList*> pluginListOf (Project& p, const std::string& target)
{
    if (Project::isMaster (target))
        return &p.edit().getMasterPluginList();

    if (auto* t = p.findTrack (target))
        return &t->pluginList;

    return makeError (errc::notFound, "no track " + target, json { { "track_id", target } });
}

//==============================================================================
void registerProjectGet (CommandRegistry& r)
{
    r.add (makeProjectCommand ("project.get",
        obj ({ { "include", arrayOf ({ { "type", "string" }, { "enum", { "tracks", "items", "markers", "tempo", "fx" } } }) } }),
        anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto include = params.value ("include", json::array());
            const auto wants = [&] (const char* what) { return include.empty() || std::find (include.begin(), include.end(), what) != include.end(); };

            json out { { "project_id", p.id() }, { "name", p.name() }, { "revision", p.revision() }, { "sample_rate", p.manifest().sampleRate },
                       { "length_s", p.edit().getLength().inSeconds() }, { "can_undo", p.edit().getUndoManager().canUndo() },
                       { "can_redo", p.edit().getUndoManager().canRedo() }, { "dirty", p.dirty() } };

            if (wants ("tracks"))
            {
                json tracks = json::array();
                int index = 0;

                for (auto* t : te::getAllTracks (p.edit()))
                {
                    auto* at = dynamic_cast<te::AudioTrack*> (t);

                    if (at == nullptr && dynamic_cast<te::FolderTrack*> (t) == nullptr)
                        continue;

                    json tj { { "track_id", Project::idOf (t->state) }, { "index", index++ }, { "name", toStd (t->getName()) }, { "color", colourHex (t->getColour()) },
                              { "mute", (bool) t->state.getProperty (te::IDs::mute, false) }, { "solo", (bool) t->state.getProperty (te::IDs::solo, false) } };

                    if (at != nullptr && at->getVolumePlugin() != nullptr)
                    {
                        tj["volume_db"] = (double) te::volumeFaderPositionToDB (at->getVolumePlugin()->volume.get());
                        tj["pan"] = (double) at->getVolumePlugin()->pan.get();
                    }

                    if (auto* ct = dynamic_cast<te::ClipTrack*> (t))
                        tj["item_count"] = ct->getClips().size();

                    tracks.push_back (tj);
                }

                out["tracks"] = tracks;

                if (auto vp = p.edit().getMasterVolumePlugin())
                    out["master"] = { { "track_id", "master" }, { "volume_db", (double) te::volumeFaderPositionToDB (vp->volume.get()) }, { "pan", (double) vp->pan.get() } };
            }

            if (wants ("items"))
            {
                json items = json::array();

                for (auto* ct : te::getClipTracks (p.edit()))
                    for (auto* c : ct->getClips())
                        items.push_back (clipJson (p, *c));

                out["items"] = items;
            }

            if (wants ("markers"))
            {
                json markers = json::array();

                for (auto* m : p.edit().getMarkerManager().getMarkers())
                {
                    const double s = m->getPosition().getStart().inSeconds();
                    markers.push_back ({ { "marker_id", Project::idOf (m->state) }, { "name", toStd (m->getName()) }, { "time_s", s }, { "bar_beat", barBeatString (p.edit(), s) } });
                }

                out["markers"] = markers;
            }

            if (wants ("tempo"))
            {
                auto& ts = p.edit().tempoSequence;
                json points = json::array();

                for (int i = 0; i < ts.getNumTempos(); ++i)
                {
                    auto* t = ts.getTempo (i);
                    const double s = ts.toTime (t->getStartBeat()).inSeconds();
                    points.push_back ({ { "bpm", t->getBpm() }, { "time_s", s }, { "qn", t->getStartBeat().inBeats() }, { "bar_beat", barBeatString (p.edit(), s) } });
                }

                const auto& sig = ts.getTimeSigAt (te::TimePosition());
                out["tempo"] = { { "points", points }, { "time_signature", { { "numerator", sig.numerator.get() }, { "denominator", sig.denominator.get() } } } };
            }

            if (wants ("fx"))
            {
                json fx = json::object();

                for (auto* t : te::getAudioTracks (p.edit()))
                {
                    json list = json::array();

                    for (auto* pl : t->pluginList.getPlugins())
                        list.push_back (pluginInfo (*pl, false));

                    fx[Project::idOf (t->state)] = list;
                }

                json masterList = json::array();

                for (auto* pl : p.edit().getMasterPluginList().getPlugins())
                    masterList.push_back (pluginInfo (*pl, false));

                fx["master"] = masterList;
                out["fx"] = fx;
            }

            return out;
        }));
}

//==============================================================================
void registerItemCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("item.list",
        obj ({ { "track_id", strMin (1) }, { "from_s", numMin (0) }, { "to_s", numMin (0) } }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            json items = json::array();
            const std::string trackId = params.value ("track_id", "");

            if (! trackId.empty() && p.findTrack (trackId) == nullptr)
                return makeError (errc::notFound, "no track " + trackId, json { { "track_id", trackId } });

            for (auto* ct : te::getClipTracks (p.edit()))
            {
                if (! trackId.empty() && Project::idOf (ct->state) != trackId)
                    continue;

                for (auto* c : ct->getClips())
                {
                    const double s = c->getPosition().getStart().inSeconds(), e = c->getPosition().getEnd().inSeconds();

                    if ((params.contains ("from_s") && e <= params["from_s"].get<double>()) || (params.contains ("to_s") && s >= params["to_s"].get<double>()))
                        continue;

                    items.push_back (clipJson (p, *c));
                }
            }

            return json { { "revision", p.revision() }, { "items", items } };
        }));

    r.add (makeProjectCommand ("item.update",
        obj ({ { "item_id", strMin (1) }, { "name", strMin (1) }, { "start_s", numMin (0) }, { "start", timePosSchema() }, { "length_s", numMin (0.001) },
               { "length", durationSchema() }, { "mute", boolean() },
               { "trim_start_s", numMin (0) }, { "trim_end_s", numMin (0) }, { "fade_in_s", numMin (0) }, { "fade_out_s", numMin (0) }, { "gain_db", { { "type", "number" }, { "minimum", -100 }, { "maximum", 24 } } } },
             { "item_id" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* clip = findClipById (p, params["item_id"].get<std::string>());

            if (clip == nullptr)
                return makeError (errc::notFound, "no item " + params["item_id"].get<std::string>(), json { { "item_id", params["item_id"] } });

            auto start = readTime (p, params, "start");

            if (! start.ok())
                return start.error();

            std::optional<double> length;

            if (params.contains ("length_s"))
            {
                length = params["length_s"].get<double>();
            }
            else if (params.contains ("length"))
            {
                auto l = durationToSeconds (p, params["length"], start.value().value_or (clip->getPosition().getStart().inSeconds()));

                if (! l.ok())
                    return l.error();

                length = l.value();
            }

            if (length && *length < 0.001)
                return makeError (errc::invalidParams, "length must be at least 1 ms", json { { "path", "/length_s" } });

            if (params.contains ("name"))
                clip->setName (toJuce (params["name"].get<std::string>()));

            if (params.contains ("mute"))
                clip->setMuted (params["mute"].get<bool>());

            if (start.value())
                clip->setStart (te::TimePosition::fromSeconds (*start.value()), false, false);

            if (length)
                clip->setLength (te::TimeDuration::fromSeconds (*length), false);

            // trimming keeps the audio where it is on the timeline: only the visible window changes
            if (params.contains ("trim_start_s"))
            {
                const auto end = clip->getPosition().getEnd();
                const auto s = te::TimePosition::fromSeconds (params["trim_start_s"].get<double>());

                if (s >= end)
                    return makeError (errc::invalidParams, "trim_start_s must be before the end of the item", json { { "path", "/trim_start_s" } });

                clip->setStart (s, true, false);
            }

            if (params.contains ("trim_end_s"))
            {
                const auto e = te::TimePosition::fromSeconds (params["trim_end_s"].get<double>());

                if (e <= clip->getPosition().getStart())
                    return makeError (errc::invalidParams, "trim_end_s must be after the start of the item", json { { "path", "/trim_end_s" } });

                clip->setEnd (e, true);
            }

            if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
            {
                if (params.contains ("gain_db"))
                    audio->setGainDB (params["gain_db"].get<float>());

                const double lengthNow = audio->getPosition().getLength().inSeconds();

                if (params.contains ("fade_in_s"))
                    audio->setFadeIn (te::TimeDuration::fromSeconds (std::min (params["fade_in_s"].get<double>(), lengthNow)));

                if (params.contains ("fade_out_s"))
                    audio->setFadeOut (te::TimeDuration::fromSeconds (std::min (params["fade_out_s"].get<double>(), lengthNow)));
            }
            else if (params.contains ("gain_db") || params.contains ("fade_in_s") || params.contains ("fade_out_s"))
            {
                return makeError (errc::invalidParams, "gain and fades apply to audio items only");
            }

            return json { { "item", clipJson (p, *clip) } };
        }));

    r.add (makeProjectCommand ("item.split",
        obj ({ { "item_id", strMin (1) }, { "at_s", numMin (0) }, { "at", timePosSchema() } }, { "item_id" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* clip = findClipById (p, params["item_id"].get<std::string>());

            if (clip == nullptr)
                return makeError (errc::notFound, "no item " + params["item_id"].get<std::string>(), json { { "item_id", params["item_id"] } });

            auto at = readTime (p, params, "at");

            if (! at.ok())
                return at.error();

            if (! at.value())
                return makeError (errc::invalidParams, "give at_s or at", json { { "path", "/at" } });

            const auto pos = te::TimePosition::fromSeconds (*at.value());
            const auto range = clip->getPosition().time;

            if (pos <= range.getStart() || pos >= range.getEnd())
                return makeError (errc::invalidParams, "the split point is outside the item", json { { "path", "/at" }, { "item_start_s", range.getStart().inSeconds() }, { "item_end_s", range.getEnd().inSeconds() } });

            auto* track = clip->getClipTrack();

            if (track == nullptr)
                return makeError (errc::preconditionFailed, "the item is not on a track");

            const auto leftId = Project::idOf (clip->state);
            auto* right = track->splitClip (*clip, pos);

            if (right == nullptr)
                return makeError (errc::internal, "the item could not be split");

            ChangeCollector::assignMissingIds (right->state);
            return json { { "left", clipJson (p, *clip) }, { "right", clipJson (p, *right) }, { "left_id", leftId }, { "right_id", Project::idOf (right->state) } };
        }));

    r.add (makeProjectCommand ("item.delete", obj ({ { "item_id", strMin (1) } }, { "item_id" }), anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* clip = findClipById (p, params["item_id"].get<std::string>());

            if (clip == nullptr)
                return makeError (errc::notFound, "no item " + params["item_id"].get<std::string>(), json { { "item_id", params["item_id"] } });

            clip->removeFromParent();
            return json { { "item_id", params["item_id"] } };
        }));
}

//==============================================================================
void registerTimeCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("time.convert", obj ({ { "time", timePosSchema() } }, { "time" }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto s = timePosToSeconds (p, params["time"]);

            if (! s.ok())
                return s.error();

            auto j = describeTime (p.edit(), s.value());
            const auto bb = p.edit().tempoSequence.toBarsAndBeats (te::TimePosition::fromSeconds (s.value()));
            j["bar"] = bb.bars + 1;
            j["beat"] = bb.beats.inBeats() + 1.0;
            return j;
        }));

    r.add (makeProjectCommand ("tempo.get", obj ({}), anyObject(), false,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            auto& ts = p.edit().tempoSequence;
            json points = json::array();

            for (int i = 0; i < ts.getNumTempos(); ++i)
            {
                auto* t = ts.getTempo (i);
                const double s = ts.toTime (t->getStartBeat()).inSeconds();
                points.push_back ({ { "bpm", t->getBpm() }, { "time_s", s }, { "qn", t->getStartBeat().inBeats() }, { "bar_beat", barBeatString (p.edit(), s) } });
            }

            json sigs = json::array();

            for (auto* sig : ts.getTimeSigs())
                sigs.push_back ({ { "numerator", sig->numerator.get() }, { "denominator", sig->denominator.get() } });

            return json { { "revision", p.revision() }, { "points", points }, { "time_signatures", sigs } };
        }));

    // Replaces the tempo points. The first point is the tempo at the start of the project; later points are placed by bar or qn.
    r.add (makeProjectCommand ("tempo.set",
        obj ({ { "points", { { "type", "array" }, { "minItems", 1 }, { "maxItems", 512 },
                             { "items", obj ({ { "bpm", { { "type", "number" }, { "minimum", 20 }, { "maximum", 400 } } }, { "bar", { { "type", "integer" }, { "minimum", 1 } } },
                                               { "qn", numMin (0) } }, { "bpm" }) } } } }, { "points" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto& ts = p.edit().tempoSequence;
            std::vector<std::pair<double, double>> pts;      // qn, bpm

            for (auto& pt : params["points"])
            {
                double qn = 0.0;

                if (pt.contains ("qn"))
                {
                    qn = pt["qn"].get<double>();
                }
                else if (pt.contains ("bar"))
                {
                    auto s = timePosToSeconds (p, { { "bar", pt["bar"] } });

                    if (! s.ok())
                        return s.error();

                    qn = ts.toBeats (te::TimePosition::fromSeconds (s.value())).inBeats();
                }

                pts.emplace_back (qn, pt["bpm"].get<double>());
            }

            std::sort (pts.begin(), pts.end());

            if (pts.front().first > 1.0e-9)
                return makeError (errc::invalidParams, "the first tempo point must be at the start (omit bar/qn, or bar 1)", json { { "path", "/points/0" } });

            for (size_t i = 1; i < pts.size(); ++i)
                if (pts[i].first - pts[i - 1].first < 1.0e-9)
                    return makeError (errc::invalidParams, "two tempo points at the same position", json { { "path", "/points/" + std::to_string (i) } });

            // positions are given in quarter notes: convert before the sequence changes, keep the beat positions (not the times)
            while (ts.getNumTempos() > 1)
                ts.removeTempo (ts.getNumTempos() - 1, false);

            ts.getTempo (0)->setBpm (pts.front().second);

            for (size_t i = 1; i < pts.size(); ++i)
                ts.insertTempo (te::BeatPosition::fromBeats (pts[i].first), pts[i].second, 1.0f);

            json out = json::array();

            for (int i = 0; i < ts.getNumTempos(); ++i)
                out.push_back ({ { "bpm", ts.getTempo (i)->getBpm() }, { "qn", ts.getTempo (i)->getStartBeat().inBeats() } });

            return json { { "points", out } };
        }));
}

//==============================================================================
void registerMarkerUpsert (CommandRegistry& r)
{
    r.add (makeProjectCommand ("marker.upsert",
        obj ({ { "markers", { { "type", "array" }, { "minItems", 1 }, { "maxItems", 500 },
                              { "items", obj ({ { "marker_id", strMin (1) }, { "name", str() }, { "time_s", numMin (0) }, { "time", timePosSchema() } }) } } } }, { "markers" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto& mm = p.edit().getMarkerManager();
            json result = json::array();
            size_t index = 0;

            for (auto& m : params["markers"])
            {
                const auto where = "/markers/" + std::to_string (index++);
                auto t = readTime (p, m, "time");

                if (! t.ok())
                    return t.error();

                te::MarkerClip* clip = nullptr;

                if (m.contains ("marker_id"))
                {
                    for (auto* c : mm.getMarkers())
                        if (Project::idOf (c->state) == m["marker_id"].get<std::string>())
                            clip = c;

                    if (clip == nullptr)
                        return makeError (errc::notFound, "no marker " + m["marker_id"].get<std::string>(), json { { "path", where } });
                }
                else
                {
                    if (! t.value())
                        return makeError (errc::invalidParams, "a new marker needs time_s or time", json { { "path", where } });

                    clip = dynamic_cast<te::MarkerClip*> (mm.createMarker (mm.getNextUniqueID(), te::TimePosition::fromSeconds (*t.value()), te::TimeDuration(), nullptr).get());

                    if (clip == nullptr)
                        return makeError (errc::internal, "marker could not be created");

                    ChangeCollector::assignMissingIds (clip->state);
                }

                if (m.contains ("name"))
                    clip->setName (toJuce (m["name"].get<std::string>()));

                if (t.value() && m.contains ("marker_id"))
                    clip->setStart (te::TimePosition::fromSeconds (*t.value()), false, true);

                const double s = clip->getPosition().getStart().inSeconds();
                result.push_back ({ { "marker_id", Project::idOf (clip->state) }, { "name", toStd (clip->getName()) }, { "time_s", s }, { "bar_beat", barBeatString (p.edit(), s) } });
            }

            return json { { "markers", result } };
        }));
}

//==============================================================================
void registerFxExtraCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("fx.list", obj ({ { "target", strMin (1) } }, { "target" }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto list = pluginListOf (p, params["target"].get<std::string>());

            if (! list.ok())
                return list.error();

            json out = json::array();
            int index = 0;

            for (auto* pl : list.value()->getPlugins())
            {
                auto j = pluginInfo (*pl, false);
                j["index"] = index++;
                j["is_fader"] = dynamic_cast<te::VolumeAndPanPlugin*> (pl) != nullptr;
                out.push_back (j);
            }

            return json { { "revision", p.revision() }, { "target", params["target"] }, { "fx", out } };
        }));

    r.add (makeProjectCommand ("fx.params", obj ({ { "plugin_id", strMin (1) } }, { "plugin_id" }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* plugin = p.findPlugin (params["plugin_id"].get<std::string>());

            if (plugin == nullptr)
                return makeError (errc::notFound, "no plugin " + params["plugin_id"].get<std::string>());

            return json { { "revision", p.revision() }, { "fx", pluginInfo (*plugin, true) } };
        }));

    // params: { "<ident or name>": <number in the parameter's own unit> | "<text such as -6 dB>" }
    r.add (makeProjectCommand ("fx.update",
        obj ({ { "plugin_id", strMin (1) }, { "enabled", boolean() }, { "params", anyObject() } }, { "plugin_id" }), anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* plugin = p.findPlugin (params["plugin_id"].get<std::string>());

            if (plugin == nullptr)
                return makeError (errc::notFound, "no plugin " + params["plugin_id"].get<std::string>());

            if (params.contains ("params"))
            {
                for (auto it = params["params"].begin(); it != params["params"].end(); ++it)
                {
                    te::AutomatableParameter* found = nullptr;

                    for (auto* ap : plugin->getAutomatableParameters())
                        if (toStd (ap->paramID) == it.key() || toStd (ap->getParameterName()).compare (it.key()) == 0)
                            found = ap;

                    if (found == nullptr)
                        for (auto* ap : plugin->getAutomatableParameters())
                            if (toJuce (it.key()).equalsIgnoreCase (ap->getParameterName()))
                                found = ap;

                    if (found == nullptr)
                        return makeError (errc::notFound, "plugin has no parameter " + it.key(), json { { "path", "/params/" + it.key() } });

                    float value = 0.0f;

                    if (it.value().is_number())
                        value = it.value().get<float>();
                    else if (it.value().is_string())
                        value = found->stringToValue (toJuce (it.value().get<std::string>()));
                    else
                        return makeError (errc::invalidParams, "a parameter value is a number or a string such as \"-6 dB\"", json { { "path", "/params/" + it.key() } });

                    const auto range = found->getValueRange();

                    if (value < range.getStart() || value > range.getEnd())
                        return makeError (errc::invalidParams, it.key() + " must be within " + std::to_string (range.getStart()) + " .. " + std::to_string (range.getEnd()),
                                          json { { "path", "/params/" + it.key() }, { "min", range.getStart() }, { "max", range.getEnd() } });

                    found->setParameter (value, juce::sendNotification);
                }
            }

            if (params.contains ("enabled"))
                plugin->setEnabled (params["enabled"].get<bool>());

            return json { { "fx", pluginInfo (*plugin, true) } };
        }));
}
} // namespace

void registerEditCommands (CommandRegistry& r)
{
    registerProjectGet (r);
    registerItemCommands (r);
    registerTimeCommands (r);
    registerMarkerUpsert (r);
    registerFxExtraCommands (r);
}

} // namespace rovox
