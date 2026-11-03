// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// M4 commands (task 210 reserved names): routing.list / send_create / send_update / send_delete (aux sends to a return track, for reverb or
// parallel buses) and track.duplicate. A send is a plugin on the source track (id `snd_...`), the return is a plugin on the bus track.
#include "Command.h"
#include "FadeStage.h"

#include "ProjectManager.h"

#include <algorithm>
#include <cmath>
#include <set>

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
json boolean()                      { return { { "type", "boolean" } }; }
json anyObject()                    { return { { "type", "object" } }; }
json gainSchema()                   { return { { "type", "number" }, { "minimum", -100 }, { "maximum", 12 } }; }

te::AudioTrack* audioTrack (Project& p, const std::string& id)
{
    return dynamic_cast<te::AudioTrack*> (p.findTrack (id));
}

Error trackNotFound (const std::string& id)     { return makeError (errc::notFound, "no track " + id, json { { "track_id", id } }); }

te::AuxReturnPlugin* returnOf (te::AudioTrack& t)
{
    return t.pluginList.findFirstPluginOfType<te::AuxReturnPlugin>();
}

te::AuxReturnPlugin* returnForBus (Project& p, int bus)
{
    for (auto* t : te::getAudioTracks (p.edit()))
        for (auto* r : t->pluginList.getPluginsOfType<te::AuxReturnPlugin>())
            if (r->busNumber.get() == bus)
                return r;

    return nullptr;
}

te::AudioTrack* trackOfPlugin (te::Plugin& pl)
{
    return dynamic_cast<te::AudioTrack*> (pl.getOwnerTrack());
}

bool isPreFader (te::AudioTrack& t, te::AuxSendPlugin& s)
{
    return t.getVolumePlugin() != nullptr && t.pluginList.indexOf (&s) < t.pluginList.indexOf (t.getVolumePlugin());
}

json sendJson (Project& p, te::AuxSendPlugin& s)
{
    auto* src = trackOfPlugin (s);
    auto* ret = returnForBus (p, s.busNumber.get());
    auto* dst = ret != nullptr ? trackOfPlugin (*ret) : nullptr;
    json j { { "send_id", Project::idOf (s.state) }, { "bus", s.busNumber.get() }, { "bus_name", toStd (s.getBusName()) }, { "gain_db", (double) te::volumeFaderPositionToDB (s.gainLevel.get()) },
             { "mute", s.isMute() }, { "pre_fader", src != nullptr && isPreFader (*src, s) } };

    if (src != nullptr)     j["track_id"] = Project::idOf (src->state);
    if (dst != nullptr)     j["to_track_id"] = Project::idOf (dst->state);
    else                    j["warning"] = "no return track listens to this bus: the send is silent";
    return j;
}

/** True if audio of `from` already reaches `to` through sends (so a send to -> from would be a feedback loop). */
bool reaches (Project& p, te::AudioTrack& from, te::AudioTrack& to, std::set<te::AudioTrack*>& seen)
{
    if (&from == &to)
        return true;

    if (! seen.insert (&from).second)
        return false;

    for (auto* s : from.pluginList.getPluginsOfType<te::AuxSendPlugin>())
        if (auto* ret = returnForBus (p, s->busNumber.get()))
            if (auto* dst = trackOfPlugin (*ret))
                if (reaches (p, *dst, to, seen))
                    return true;

    return false;
}

te::AuxSendPlugin* findSend (Project& p, const std::string& id)
{
    for (auto* t : te::getAudioTracks (p.edit()))
        for (auto* s : t->pluginList.getPluginsOfType<te::AuxSendPlugin>())
            if (Project::idOf (s->state) == id)
                return s;

    return nullptr;
}

bool moveBeforeFader (te::AudioTrack& t, te::AuxSendPlugin& s, bool pre, juce::UndoManager* um)
{
    auto* fader = t.getVolumePlugin();

    if (fader == nullptr || isPreFader (t, s) == pre)
        return false;

    auto parent = s.state.getParent();
    const int from = parent.indexOf (s.state);

    if (pre)
        parent.moveChild (from, parent.indexOf (fader->state), um);
    else
        parent.moveChild (from, parent.indexOf (fader->state) + 1 > parent.getNumChildren() - 1 ? parent.getNumChildren() - 1 : parent.indexOf (fader->state) + 1, um);

    return true;
}
} // namespace

void registerRoutingCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("routing.list", obj ({ { "track_id", strMin (1) } }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            const std::string only = params.value ("track_id", "");

            if (! only.empty() && audioTrack (p, only) == nullptr)
                return trackNotFound (only);

            json sends = json::array(), returns = json::array();

            for (auto* t : te::getAudioTracks (p.edit()))
            {
                for (auto* s : t->pluginList.getPluginsOfType<te::AuxSendPlugin>())
                    if (only.empty() || Project::idOf (t->state) == only)
                        sends.push_back (sendJson (p, *s));

                if (auto* ret = returnOf (*t))
                    returns.push_back ({ { "track_id", Project::idOf (t->state) }, { "name", toStd (t->getName()) }, { "bus", ret->busNumber.get() }, { "return_id", Project::idOf (ret->state) } });
            }

            return json { { "revision", p.revision() }, { "sends", sends }, { "returns", returns } };
        }));

    r.add (makeProjectCommand ("routing.send_create",
        obj ({ { "track_id", strMin (1) }, { "to_track_id", strMin (1) }, { "gain_db", gainSchema() }, { "pre_fader", boolean() } }, { "track_id", "to_track_id" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* src = audioTrack (p, params["track_id"].get<std::string>());
            auto* dst = audioTrack (p, params["to_track_id"].get<std::string>());

            if (src == nullptr)     return trackNotFound (params["track_id"].get<std::string>());
            if (dst == nullptr)     return trackNotFound (params["to_track_id"].get<std::string>());

            if (src == dst)
                return makeError (errc::invalidParams, "a track cannot send to itself", json { { "path", "/to_track_id" } });

            std::set<te::AudioTrack*> seen;

            if (reaches (p, *dst, *src, seen))
                return makeError (errc::preconditionFailed, "that send would create a feedback loop: " + toStd (dst->getName()) + " already feeds " + toStd (src->getName()));

            auto* ret = returnOf (*dst);

            if (ret == nullptr)
            {
                std::set<int> used;

                for (auto* t : te::getAudioTracks (p.edit()))
                    for (auto* x : t->pluginList.getPluginsOfType<te::AuxReturnPlugin>())
                        used.insert (x->busNumber.get());

                int bus = 0;

                while (used.count (bus) > 0)
                    ++bus;

                auto plugin = p.edit().getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {});

                if (plugin == nullptr)
                    return makeError (errc::internal, "the return plugin could not be created");

                dst->pluginList.insertPlugin (plugin, 0, nullptr);
                ret = dynamic_cast<te::AuxReturnPlugin*> (plugin.get());
                ret->busNumber = bus;
                ChangeCollector::assignMissingIds (ret->state);
                p.edit().setAuxBusName (bus, dst->getName());
            }

            auto plugin = p.edit().getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {});

            if (plugin == nullptr)
                return makeError (errc::internal, "the send plugin could not be created");

            auto* send = dynamic_cast<te::AuxSendPlugin*> (plugin.get());
            const bool pre = params.value ("pre_fader", false);
            int index = src->pluginList.size();

            if (auto* fader = src->getVolumePlugin(); fader != nullptr && pre)
                index = src->pluginList.indexOf (fader);

            src->pluginList.insertPlugin (plugin, index, nullptr);
            send->busNumber = ret->busNumber.get();
            send->setGainDb (params.value ("gain_db", 0.0f));
            ChangeCollector::assignMissingIds (send->state);
            return json { { "send_id", Project::idOf (send->state) }, { "send", sendJson (p, *send) }, { "return_id", Project::idOf (ret->state) } };
        }));

    r.add (makeProjectCommand ("routing.send_update",
        obj ({ { "send_id", strMin (1) }, { "gain_db", gainSchema() }, { "mute", boolean() }, { "pre_fader", boolean() }, { "to_track_id", strMin (1) } }, { "send_id" }),
        anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* send = findSend (p, params["send_id"].get<std::string>());

            if (send == nullptr)
                return makeError (errc::notFound, "no send " + params["send_id"].get<std::string>());

            auto* src = trackOfPlugin (*send);

            if (params.contains ("to_track_id"))
            {
                auto* dst = audioTrack (p, params["to_track_id"].get<std::string>());

                if (dst == nullptr)
                    return trackNotFound (params["to_track_id"].get<std::string>());

                auto* ret = returnOf (*dst);

                if (ret == nullptr)
                    return makeError (errc::preconditionFailed, "track " + toStd (dst->getName()) + " is not a return yet: create a send to it with routing.send_create first");

                std::set<te::AudioTrack*> seen;

                if (src != nullptr && reaches (p, *dst, *src, seen))
                    return makeError (errc::preconditionFailed, "that send would create a feedback loop");

                send->busNumber = ret->busNumber.get();
            }

            if (params.contains ("gain_db"))    send->setGainDb (params["gain_db"].get<float>());
            if (params.contains ("mute"))       send->setMute (params["mute"].get<bool>());
            if (params.contains ("pre_fader") && src != nullptr)
                moveBeforeFader (*src, *send, params["pre_fader"].get<bool>(), &p.edit().getUndoManager());

            return json { { "send", sendJson (p, *send) } };
        }));

    r.add (makeProjectCommand ("routing.send_delete", obj ({ { "send_id", strMin (1) }, { "keep_return", boolean() } }, { "send_id" }), anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* send = findSend (p, params["send_id"].get<std::string>());

            if (send == nullptr)
                return makeError (errc::notFound, "no send " + params["send_id"].get<std::string>());

            const int bus = send->busNumber.get();
            send->deleteFromParent();
            bool removedReturn = false;

            if (! params.value ("keep_return", false))
            {
                bool stillUsed = false;

                for (auto* t : te::getAudioTracks (p.edit()))
                    for (auto* s : t->pluginList.getPluginsOfType<te::AuxSendPlugin>())
                        stillUsed = stillUsed || s->busNumber.get() == bus;

                if (! stillUsed)
                    if (auto* ret = returnForBus (p, bus))
                    {
                        ret->deleteFromParent();
                        removedReturn = true;
                    }
            }

            return json { { "send_id", params["send_id"] }, { "return_removed", removedReturn } };
        }));

    //==========================================================================
    r.add (makeProjectCommand ("track.duplicate", obj ({ { "track_id", strMin (1) }, { "name", strMin (1) }, { "with_items", boolean() } }, { "track_id" }), anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* src = audioTrack (p, params["track_id"].get<std::string>());

            if (src == nullptr)
                return trackNotFound (params["track_id"].get<std::string>());

            auto copy = p.edit().copyTrack (src, te::TrackInsertPoint (nullptr, src));
            auto* track = dynamic_cast<te::AudioTrack*> (copy.get());

            if (track == nullptr)
                return makeError (errc::internal, "the track could not be duplicated");

            ChangeCollector::assignMissingIds (track->state);

            if (! params.value ("with_items", true))
                {
                auto clips = track->getClips();

                for (auto* c : clips)
                    c->removeFromParent();
            }

            // a duplicated return would be a second listener on the bus, and a duplicated send would double the effect: leave routing to the user
            for (auto* ret : track->pluginList.getPluginsOfType<te::AuxReturnPlugin>())
                ret->deleteFromParent();

            for (auto* s : track->pluginList.getPluginsOfType<te::AuxSendPlugin>())
                s->deleteFromParent();

            track->setName (toJuce (params.value ("name", toStd (src->getName()) + " copy")));
            json plugins = json::array();

            for (auto* pl : track->pluginList.getPlugins())
                if (! isFadeStage (*pl))
                    plugins.push_back ({ { "plugin_id", Project::idOf (pl->state) }, { "name", toStd (pl->getName()) }, { "type", toStd (pl->getPluginType()) } });

            json items = json::array();

            for (auto* c : track->getClips())
                items.push_back (Project::idOf (c->state));

            return json { { "track_id", Project::idOf (track->state) }, { "source_track_id", params["track_id"] }, { "name", toStd (track->getName()) }, { "plugins", plugins }, { "items", items },
                          { "note", "sends and returns are not copied" } };
        }));
}

} // namespace rovox
