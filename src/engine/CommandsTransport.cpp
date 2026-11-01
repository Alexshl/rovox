// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Commands added in M3 for the web UI and the reserved MCP tools: transport.get / transport.set (task 330),
// mixer.get_meters (task 320), agent.accept (task 320), item.source (peaks endpoint, task 310).
#include "Command.h"

#include "Transport.h"
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

json anyObject()    { return { { "type", "object" } }; }

json meterJson (const ProjectMeters::Reading& r)
{
    return { { "track_id", r.trackId }, { "peak_db", json::array ({ r.peakDb[0], r.peakDb[1] }) } };
}
} // namespace

void registerTransportCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("transport.get", obj({}), anyObject(), false,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            return p.transport().state();
        }));

    r.add (makeProjectCommand ("transport.set",
        obj ({ { "state", { { "type", "string" }, { "enum", json::array ({ "playing", "stopped" }) } } },
               { "position_s", { { "type", "number" }, { "minimum", 0 } } },
               { "position", timePosSchema() },
               { "loop", obj ({ { "enabled", { { "type", "boolean" } } }, { "start_s", { { "type", "number" }, { "minimum", 0 } } }, { "end_s", { { "type", "number" }, { "minimum", 0 } } } }) } }),
        anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            return p.transport().apply (params);
        }));

    r.add (makeProjectCommand ("mixer.get_meters", obj({}), anyObject(), false,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            json tracks = json::array();
            json master = nullptr;

            for (auto& reading : p.meters().takePeakSinceLastCall())
            {
                if (reading.trackId == "master")
                    master = meterJson (reading);
                else
                    tracks.push_back (meterJson (reading));
            }

            return json { { "unit", "dBFS" }, { "note", "peak per channel since the previous mixer.get_meters call; -100 = silence; meters only move while the project plays" },
                          { "playing", p.transport().wantsPlaying() }, { "tracks", tracks }, { "master", master } };
        }));

    r.add (makeProjectCommand ("agent.accept", obj ({ { "up_to_revision", { { "type", "integer" }, { "minimum", 0 } } } }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext& ctx) -> Result<json>
        {
            const std::uint64_t upTo = params.contains ("up_to_revision") ? params["up_to_revision"].get<std::uint64_t>() : p.revision();
            auto& accepted = p.acceptedRevision();

            if (upTo > accepted)
                accepted = upTo;

            if (p.onEvent)
                p.onEvent ({ { "type", "agent.accepted" }, { "project_id", p.id() }, { "accepted_revision", accepted.load() }, { "origin", ctx.origin.toJson() } });

            return json { { "accepted_revision", accepted.load() } };
        }));

    r.add (makeProjectCommand ("item.source", obj ({ { "item_id", { { "type", "string" }, { "minLength", 1 } } } }, { "item_id" }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* clip = p.findClip (params["item_id"].get<std::string>());

            if (clip == nullptr)
                return makeError (errc::notFound, "no item " + params["item_id"].get<std::string>(), json { { "item_id", params["item_id"] } });

            auto* audio = dynamic_cast<te::AudioClipBase*> (clip);

            if (audio == nullptr)
                return makeError (errc::unsupported, "the item has no audio file");

            return json { { "path", toPath (audio->getOriginalFile()).string() }, { "offset_s", clip->getPosition().getOffset().inSeconds() },
                          { "length_s", clip->getPosition().getLength().inSeconds() }, { "speed", clip->getSpeedRatio() } };
        }));
}

} // namespace rovox
