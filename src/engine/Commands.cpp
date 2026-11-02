// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Command.h"

#include "Ids.h"
#include "PluginHost.h"
#include "ProjectManager.h"
#include "JsfxPlugin.h"
#include "StockPlugins.h"

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

json str()                                  { return { { "type", "string" } }; }
json strMin (int n)                         { return { { "type", "string" }, { "minLength", n } }; }
json num()                                  { return { { "type", "number" } }; }
json numRange (double lo, double hi)        { return { { "type", "number" }, { "minimum", lo }, { "maximum", hi } }; }
json integer()                              { return { { "type", "integer" } }; }
json boolean()                              { return { { "type", "boolean" } }; }

std::string colourToHex (juce::Colour c)    { return "#" + c.toDisplayString (false).toLowerCase().toStdString(); }

std::optional<juce::Colour> parseColour (const std::string& s)
{
    if (s.size() != 7 || s[0] != '#')
        return std::nullopt;

    for (size_t i = 1; i < 7; ++i)
        if (! std::isxdigit ((unsigned char) s[i]))
            return std::nullopt;

    return juce::Colour::fromString ("ff" + juce::String (s.substr (1)));
}

json pluginJson (te::Plugin& p)
{
    json j { { "plugin_id", Project::idOf (p.state) }, { "name", toStd (p.getName()) }, { "type", toStd (p.getPluginType()) }, { "enabled", p.isEnabled() } };

    if (auto* ext = dynamic_cast<te::ExternalPlugin*> (&p))
    {
        j["loaded"] = ext->getAudioPluginInstance() != nullptr;

        if (ext->getAudioPluginInstance() == nullptr)
            j["load_error"] = toStd (ext->getLoadError());
    }

    return j;
}

json trackJson (Project& project, te::Track& t)
{
    json j { { "track_id", Project::idOf (t.state) }, { "name", toStd (t.getName()) }, { "color", colourToHex (t.getColour()) },
             { "mute", (bool) t.state.getProperty (te::IDs::mute, false) }, { "solo", (bool) t.state.getProperty (te::IDs::solo, false) } };

    if (auto* at = dynamic_cast<te::AudioTrack*> (&t))
    {
        // read the tree-backed values: the plugin's parameter objects follow the tree asynchronously (stale right after an undo)
        auto* vp = at->getVolumePlugin();
        j["volume_db"] = vp != nullptr ? (double) te::volumeFaderPositionToDB (vp->volume.get()) : 0.0;
        j["pan"] = vp != nullptr ? (double) vp->pan.get() : 0.0;
    }

    json items = json::array(), plugins = json::array();

    if (auto* ct = dynamic_cast<te::ClipTrack*> (&t))
        for (auto* c : ct->getClips())
            items.push_back ({ { "item_id", Project::idOf (c->state) }, { "name", toStd (c->getName()) },
                               { "start_s", c->getPosition().getStart().inSeconds() }, { "length_s", c->getPosition().getLength().inSeconds() } });

    for (auto* p : t.pluginList.getPlugins())
        plugins.push_back (pluginJson (*p));

    j["items"] = items;
    j["plugins"] = plugins;
    (void) project;
    return j;
}

/** Tracktion writes mute / solo through a CachedValue without an UndoManager, so they would not be part of the undo step. */
void setMuteUndoable (te::Track& t, bool on)    { t.state.setProperty (te::IDs::mute, on, &t.edit.getUndoManager()); }
void setSoloUndoable (te::Track& t, bool on)    { t.state.setProperty (te::IDs::solo, on, &t.edit.getUndoManager()); }

Error trackNotFound (const std::string& id)     { return makeError (errc::notFound, "no track " + id, json { { "track_id", id } }); }

te::AudioTrack* audioTrack (Project& p, const std::string& id)
{
    return dynamic_cast<te::AudioTrack*> (p.findTrack (id));
}

//==============================================================================
void registerProjectCommands (CommandRegistry& r)
{
    r.add (makeGlobalCommand ("project.new",
        obj ({ { "name", str() }, { "ephemeral", boolean() }, { "ttl_s", { { "type", "integer" }, { "minimum", 1 } } }, { "sample_rate", numRange (8000, 192000) } }),
        obj ({ { "project_id", str() } }), false,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            CreateOptions o;
            o.name = p.value ("name", "Untitled");
            o.tenant = ctx.tenant;
            o.ephemeral = p.value ("ephemeral", false);
            o.sampleRate = p.value ("sample_rate", 44100.0);

            if (p.contains ("ttl_s"))
                o.ttlS = p["ttl_s"].get<int>();

            auto id = pm.create (o);

            if (! id.ok())
                return id.error();

            return json { { "project_id", id.value() } };
        }));

    r.add (makeGlobalCommand ("project.list", obj ({ { "include_ephemeral", boolean() } }), obj ({ { "projects", { { "type", "array" } } } }), false,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            json out = json::array();

            for (auto& s : pm.list (ctx.tenant, p.value ("include_ephemeral", false)))
                out.push_back (s.toJson());

            return json { { "projects", out } };
        }));

    r.add (makeGlobalCommand ("project.catalog", obj ({ { "include_ephemeral", boolean() } }), obj ({ { "projects", { { "type", "array" } } } }), false,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            return json { { "projects", pm.catalog (ctx.tenant, p.value ("include_ephemeral", false)) } };
        }));

    r.add (makeGlobalCommand ("project.open", obj ({ { "target", strMin (1) } }, { "target" }), obj ({ { "project_id", str() } }), false,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            auto id = pm.open (p["target"].get<std::string>(), { .tenant = ctx.tenant });

            if (! id.ok())
                return id.error();

            return json { { "project_id", id.value() } };
        }));

    r.add (makeGlobalCommand ("project.close", obj ({ { "project_id", strMin (1) }, { "save", boolean() } }, { "project_id" }), obj ({}), false,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            const auto id = p["project_id"].get<std::string>();

            if (pm.find (id, ctx.tenant) == nullptr)
                return makeError (errc::notFound, "no open project " + id);

            auto res = pm.close (id, p.value ("save", true) ? SaveMode::save : SaveMode::discard);

            if (! res.ok())
                return res.error();

            return json::object();
        }));

    // Ephemeral projects (embedding): deletion by the TTL sweeper is a system-origin delete
    r.add (makeGlobalCommand ("project.delete", obj ({ { "project_id", strMin (1) } }, { "project_id" }), obj ({}), true,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            auto res = pm.remove (p["project_id"].get<std::string>(), ctx.tenant);

            if (! res.ok())
                return res.error();

            return json::object();
        }));

    r.add (makeGlobalCommand ("project.set_ttl", obj ({ { "project_id", strMin (1) }, { "ttl_s", { { "type", "integer" }, { "minimum", 1 } } }, { "ephemeral", boolean() } }, { "project_id", "ttl_s" }),
        obj ({ { "ttl_s", integer() } }), true,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            auto* project = pm.find (p["project_id"].get<std::string>(), ctx.tenant);

            if (project == nullptr)
                return makeError (errc::notFound, "no open project");

            project->manifest().ttlS = p["ttl_s"].get<int>();
            project->manifest().ephemeral = p.value ("ephemeral", true);
            project->commit (ctx.origin, ctx.origin.label + ": project.set_ttl", { { "edit", "", "update", { "ttl_s" } } });
            auto saved = project->save();

            if (! saved.ok())
                return saved.error();

            return json { { "ttl_s", p["ttl_s"] } };
        }));

    r.add (makeProjectCommand ("project.info", obj ({}), obj ({}), false,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            return json { { "project_id", p.id() }, { "name", p.name() }, { "tenant_id", p.tenant() }, { "revision", p.revision() }, { "dirty", p.dirty() },
                          { "sample_rate", p.manifest().sampleRate }, { "tracks", te::getAudioTracks (p.edit()).size() },
                          { "length_s", p.edit().getLength().inSeconds() }, { "path", p.dir().string() },
                          { "can_undo", p.edit().getUndoManager().canUndo() }, { "can_redo", p.edit().getUndoManager().canRedo() } };
        }));

    r.add (makeProjectCommand ("project.update", obj ({ { "name", strMin (1) }, { "delivery_hold", boolean() } }), obj ({ { "name", str() }, { "delivery_hold", boolean() } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            static const juce::Identifier holdId ("rovox:deliveryHold");

            // "don't send yet": delivery.send queues instead of sending while this is on (undoable like the name)
            if (params.contains ("delivery_hold"))
                p.edit().state.setProperty (holdId, params["delivery_hold"].get<bool>(), &p.edit().getUndoManager());

            // the manifest is not part of the Edit tree: the name is kept as an Edit property so that the change is undoable and reported, and copied to the manifest on save
            if (params.contains ("name"))
            {
                static const juce::Identifier nameId ("rovox:name");
                p.edit().state.setProperty (nameId, toJuce (params["name"].get<std::string>()), &p.edit().getUndoManager());
            }

            return json { { "name", p.name() }, { "delivery_hold", (bool) p.edit().state.getProperty (holdId, false) } };
        }));

    r.add (makeProjectCommand ("project.save", obj ({}), obj ({ { "revision", integer() } }), false,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            auto s = p.save();

            if (! s.ok())
                return s.error();

            return json { { "revision", p.revision() } };
        }));

    r.add (makeProjectCommand ("project.activity", obj ({ { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 1000 } } } }), obj ({}), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            return json { { "activity", p.activity().list ((size_t) params.value ("limit", 100)) } };
        }));
}

//==============================================================================
void registerTrackCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("track.list", obj ({}), obj ({ { "tracks", { { "type", "array" } } } }), false,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            json tracks = json::array();

            for (auto* t : te::getAllTracks (p.edit()))
                if (dynamic_cast<te::AudioTrack*> (t) != nullptr || dynamic_cast<te::FolderTrack*> (t) != nullptr)
                    tracks.push_back (trackJson (p, *t));

            json out { { "revision", p.revision() }, { "tracks", tracks } };

            if (auto vp = p.edit().getMasterVolumePlugin())
                out["master"] = { { "track_id", "master" }, { "name", "Master" }, { "volume_db", (double) te::volumeFaderPositionToDB (vp->volume.get()) }, { "pan", (double) vp->pan.get() } };

            return out;
        }));

    r.add (makeProjectCommand ("track.create",
        obj ({ { "name", strMin (1) }, { "volume_db", numRange (-100, 24) }, { "pan", numRange (-1, 1) }, { "color", str() }, { "mute", boolean() }, { "solo", boolean() } }),
        obj ({ { "track_id", str() } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            if (params.contains ("color") && ! parseColour (params["color"].get<std::string>()))
                return makeError (errc::invalidParams, "color must be #rrggbb", json { { "path", "/color" } });

            auto track = p.edit().insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (p.edit()), nullptr);

            if (track == nullptr)      // Edit limit reached: never retry in a loop (spike P5: ensureNumberOfAudioTracks spins forever)
                return makeError (errc::preconditionFailed, "cannot add a track: the Edit limit of "
                                                               + std::to_string (p.host().engine().getEngineBehaviour().getEditLimits().maxNumTracks) + " tracks is reached");

            track->setName (toJuce (params.value ("name", "Track " + std::to_string (te::getAudioTracks (p.edit()).size()))));

            if (params.contains ("volume_db"))  track->getVolumePlugin()->setVolumeDb (params["volume_db"].get<float>());
            if (params.contains ("pan"))        track->getVolumePlugin()->setPan (params["pan"].get<float>());
            if (params.contains ("color"))      track->setColour (*parseColour (params["color"].get<std::string>()));
            if (params.value ("mute", false))   setMuteUndoable (*track, true);
            if (params.value ("solo", false))   setSoloUndoable (*track, true);

            return json { { "track_id", Project::idOf (track->state) } };
        }));

    r.add (makeProjectCommand ("track.update",
        obj ({ { "track_id", strMin (1) }, { "name", strMin (1) }, { "volume_db", numRange (-100, 24) }, { "pan", numRange (-1, 1) }, { "color", str() }, { "mute", boolean() }, { "solo", boolean() } },
              { "track_id" }),
        obj ({ { "track_id", str() } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            const auto id = params["track_id"].get<std::string>();

            if (Project::isMaster (id))
            {
                auto vp = p.edit().getMasterVolumePlugin();

                if (vp == nullptr)
                    return makeError (errc::preconditionFailed, "the project has no master fader");

                for (auto* key : { "name", "color", "mute", "solo" })
                    if (params.contains (key))
                        return makeError (errc::invalidParams, std::string (key) + " cannot be changed on the master track", json { { "path", std::string ("/") + key } });

                if (params.contains ("volume_db"))  vp->setVolumeDb (params["volume_db"].get<float>());
                if (params.contains ("pan"))        vp->setPan (params["pan"].get<float>());

                return json { { "track_id", id } };
            }

            auto* track = audioTrack (p, id);

            if (track == nullptr)
                return trackNotFound (id);

            if (params.contains ("color") && ! parseColour (params["color"].get<std::string>()))
                return makeError (errc::invalidParams, "color must be #rrggbb", json { { "path", "/color" } });

            if (params.contains ("name"))       track->setName (toJuce (params["name"].get<std::string>()));
            if (params.contains ("volume_db"))  track->getVolumePlugin()->setVolumeDb (params["volume_db"].get<float>());
            if (params.contains ("pan"))        track->getVolumePlugin()->setPan (params["pan"].get<float>());
            if (params.contains ("color"))      track->setColour (*parseColour (params["color"].get<std::string>()));
            if (params.contains ("mute"))       setMuteUndoable (*track, params["mute"].get<bool>());
            if (params.contains ("solo"))       setSoloUndoable (*track, params["solo"].get<bool>());

            return json { { "track_id", id } };
        }));

    r.add (makeProjectCommand ("track.delete", obj ({ { "track_id", strMin (1) } }, { "track_id" }), obj ({ { "track_id", str() } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            const auto id = params["track_id"].get<std::string>();
            auto* track = audioTrack (p, id);

            if (track == nullptr)
                return trackNotFound (id);

            p.edit().deleteTrack (track);
            return json { { "track_id", id } };
        }));

    r.add (makeProjectCommand ("track.move", obj ({ { "track_id", strMin (1) }, { "to_index", { { "type", "integer" }, { "minimum", 0 } } } }, { "track_id", "to_index" }),
        obj ({ { "track_id", str() } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            const auto id = params["track_id"].get<std::string>();
            auto* track = audioTrack (p, id);

            if (track == nullptr)
                return trackNotFound (id);

            std::vector<te::AudioTrack*> others;

            for (auto* t : te::getAudioTracks (p.edit()))
                if (t != track)
                    others.push_back (t);

            const auto to = std::min ((size_t) params["to_index"].get<int>(), others.size());
            te::Track* preceding = to == 0 ? nullptr : others[to - 1];
            p.edit().moveTrack (track, te::TrackInsertPoint (nullptr, preceding));
            return json { { "track_id", id } };
        }));
}

//==============================================================================
std::string builtinPluginType (const std::string& name)
{
    if (name == "equaliser")    return te::EqualiserPlugin::xmlTypeName;
    if (name == "compressor")   return te::CompressorPlugin::xmlTypeName;
    if (name == "reverb")       return te::ReverbPlugin::xmlTypeName;
    if (name == "phaser")       return te::PhaserPlugin::xmlTypeName;
    if (name == "lowpass")      return te::LowPassPlugin::xmlTypeName;
    if (name == "delay")        return te::DelayPlugin::xmlTypeName;
    if (name == "chorus")       return te::ChorusPlugin::xmlTypeName;
    if (name == "fouroscillator") return te::FourOscPlugin::xmlTypeName;

    if (name.rfind ("rovox_", 0) == 0)
        if (auto* stock = findStockPlugin (name.substr (6)))
            return stock->xmlType;

    return {};
}

void registerFxCommands (CommandRegistry& r)
{
    json pluginDesc = obj ({ { "format", str() }, { "uid", str() }, { "plugin_uid", str() }, { "name", str() }, { "file", str() } });

    r.add (makeProjectCommand ("fx.add",
        obj ({ { "track_id", strMin (1) },
               { "builtin", { { "type", "string" }, { "enum", { "equaliser", "compressor", "reverb", "delay", "chorus", "phaser", "lowpass", "fouroscillator",
                                                                                      "rovox_eq", "rovox_comp", "rovox_gate", "rovox_limit", "rovox_delay", "rovox_verb" } } } },
               { "plugin", pluginDesc }, { "index", { { "type", "integer" }, { "minimum", 0 } } } }, { "track_id" }),
        obj ({ { "plugin_id", str() } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            const auto trackId = params["track_id"].get<std::string>();
            te::PluginList* list = nullptr;
            te::AudioTrack* track = nullptr;

            if (Project::isMaster (trackId))
            {
                list = &p.edit().getMasterPluginList();
            }
            else
            {
                track = audioTrack (p, trackId);

                if (track == nullptr)
                    return trackNotFound (trackId);

                list = &track->pluginList;
            }

            if (params.contains ("builtin") == params.contains ("plugin"))
                return makeError (errc::invalidParams, "give exactly one of builtin / plugin");

            te::Plugin::Ptr plugin;
            json probeInfo;

            if (params.contains ("builtin"))
            {
                plugin = p.edit().getPluginCache().createNewPlugin (toJuce (builtinPluginType (params["builtin"].get<std::string>())), {});
            }
            else
            {
                if (! params["plugin"].contains ("plugin_uid") && ! params["plugin"].contains ("uid") && ! params["plugin"].contains ("name"))
                    return makeError (errc::invalidParams, "plugin needs plugin_uid (from plugin.catalog, for example rovox:eq) or format + uid", json { { "path", "/plugin" } });

                // Rovox stock effects (plugin_uid "rovox:eq" or the name "Rovox EQ") are built in: no probe, no child process
                const StockInfo* stock = nullptr;

                for (const char* field : { "plugin_uid", "name", "uid" })
                    if (stock == nullptr && params["plugin"].contains (field) && params["plugin"][field].is_string())
                        stock = findStockPlugin (params["plugin"][field].get<std::string>());

                std::string jsfxKey;

                for (const char* field : { "plugin_uid", "uid", "name" })
                    if (jsfxKey.empty() && params["plugin"].contains (field) && params["plugin"][field].is_string() && isJsfxUid (params["plugin"][field].get<std::string>()))
                        jsfxKey = params["plugin"][field].get<std::string>();

                if (! jsfxKey.empty())
                {
                    // REAPER JS effects (plugin_uid "jsfx:<path below ROVOX_JSFX_PATH>") are built in: no probe, no child process
                    auto created = createJsfxPlugin (p.edit(), jsfxKey);

                    if (! created.ok())
                        return created.error();

                    plugin = created.value();
                }
                else if (stock != nullptr)
                {
                    plugin = p.edit().getPluginCache().createNewPlugin (toJuce (stock->xmlType), {});
                }
                else
                {
                    // catalog entries are probed in a child process first: a plugin that crashes on load never reaches this process
                    if (! params["plugin"].contains ("plugin_uid") && ! params["plugin"].contains ("uid"))
                        return makeError (errc::notFound, "no plugin named " + params["plugin"].value ("name", "") + " (plugin.catalog lists what is installed; Rovox stock effects: rovox:eq, rovox:comp, rovox:gate, rovox:limit, rovox:delay, rovox:verb)");

                    auto created = createExternalPlugin (p, params["plugin"], &probeInfo);

                    if (! created.ok())
                        return created.error();

                    plugin = created.value();
                }
            }

            if (plugin == nullptr)
                return makeError (errc::preconditionFailed, "plugin could not be created");

            int index = params.value ("index", -1);

            if (index < 0)
            {
                index = list->size();

                if (track != nullptr && track->getVolumePlugin() != nullptr)
                    index = std::max (0, list->indexOf (track->getVolumePlugin()));      // before the fader
            }

            list->insertPlugin (plugin, index, nullptr);
            plugin->initialiseFully();
            ChangeCollector::assignMissingIds (plugin->state);
            json out { { "plugin_id", Project::idOf (plugin->state) }, { "plugin", pluginJson (*plugin) } };

            if (auto* ext = dynamic_cast<te::ExternalPlugin*> (plugin.get()))
            {
                out["plugin"]["is_instrument"] = ext->isSynth();

                if (! probeInfo.is_null() && probeInfo.contains ("audible"))
                    out["plugin"]["probe"] = { { "audible", probeInfo["audible"] }, { "latency_samples", probeInfo.value ("latency_samples", 0) } };
            }

            return out;
        }));

    r.add (makeProjectCommand ("fx.remove", obj ({ { "plugin_id", strMin (1) } }, { "plugin_id" }), obj ({ { "plugin_id", str() } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            const auto id = params["plugin_id"].get<std::string>();
            auto* plugin = p.findPlugin (id);

            if (plugin == nullptr)
                return makeError (errc::notFound, "no plugin " + id);

            plugin->deleteFromParent();
            return json { { "plugin_id", id } };
        }));
}

//==============================================================================
void registerMarkerCommands (CommandRegistry& r)
{
    auto markerJson = [] (te::MarkerClip& m)
    {
        return json { { "marker_id", Project::idOf (m.state) }, { "name", toStd (m.getName()) }, { "time_s", m.getPosition().getStart().inSeconds() } };
    };

    r.add (makeProjectCommand ("marker.list", obj ({}), obj ({}), false,
        [markerJson] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            json out = json::array();

            for (auto* m : p.edit().getMarkerManager().getMarkers())
                out.push_back (markerJson (*m));

            return json { { "markers", out } };
        }));

    r.add (makeProjectCommand ("marker.create", obj ({ { "time_s", { { "type", "number" }, { "minimum", 0 } } }, { "name", str() } }, { "time_s" }),
        obj ({ { "marker_id", str() } }), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto& mm = p.edit().getMarkerManager();
            auto m = mm.createMarker (mm.getNextUniqueID(), te::TimePosition::fromSeconds (params["time_s"].get<double>()), te::TimeDuration(), nullptr);

            if (m == nullptr)
                return makeError (errc::internal, "marker could not be created");

            if (params.contains ("name"))
                m->setName (toJuce (params["name"].get<std::string>()));

            ChangeCollector::assignMissingIds (m->state);
            return json { { "marker_id", Project::idOf (m->state) } };
        }));

    auto findMarker = [] (Project& p, const std::string& id) -> te::MarkerClip*
    {
        for (auto* m : p.edit().getMarkerManager().getMarkers())
            if (Project::idOf (m->state) == id)
                return m;

        return nullptr;
    };

    r.add (makeProjectCommand ("marker.update", obj ({ { "marker_id", strMin (1) }, { "time_s", { { "type", "number" }, { "minimum", 0 } } }, { "name", str() } }, { "marker_id" }),
        obj ({ { "marker_id", str() } }), true,
        [findMarker] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* m = findMarker (p, params["marker_id"].get<std::string>());

            if (m == nullptr)
                return makeError (errc::notFound, "no marker " + params["marker_id"].get<std::string>());

            if (params.contains ("name"))
                m->setName (toJuce (params["name"].get<std::string>()));

            if (params.contains ("time_s"))
                m->setStart (te::TimePosition::fromSeconds (params["time_s"].get<double>()), false, true);

            return json { { "marker_id", params["marker_id"] } };
        }));

    r.add (makeProjectCommand ("marker.delete", obj ({ { "marker_id", strMin (1) } }, { "marker_id" }), obj ({ { "marker_id", str() } }), true,
        [findMarker] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* m = findMarker (p, params["marker_id"].get<std::string>());

            if (m == nullptr)
                return makeError (errc::notFound, "no marker " + params["marker_id"].get<std::string>());

            m->removeFromParent();
            return json { { "marker_id", params["marker_id"] } };
        }));
}

//==============================================================================
void registerHistoryCommands (CommandRegistry& r)
{
    r.add (makeProjectCommand ("undo", obj ({}), obj ({ { "undone_label", str() } }), true,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            auto& um = p.edit().getUndoManager();

            if (! um.canUndo())
                return makeError (errc::preconditionFailed, "nothing to undo");

            const auto label = toStd (um.getUndoDescription());
            um.undo();
            resyncPluginStates (p.edit());

            if (! p.undoSteps().empty())
            {
                p.redoSteps().push_back (p.undoSteps().back());
                p.undoSteps().pop_back();
            }

            return json { { "undone_label", label } };
        }, true));

    r.add (makeProjectCommand ("redo", obj ({}), obj ({ { "redone_label", str() } }), true,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            auto& um = p.edit().getUndoManager();

            if (! um.canRedo())
                return makeError (errc::preconditionFailed, "nothing to redo");

            const auto label = toStd (um.getRedoDescription());
            um.redo();
            resyncPluginStates (p.edit());

            if (! p.redoSteps().empty())
            {
                p.undoSteps().push_back (p.redoSteps().back());
                p.redoSteps().pop_back();
            }

            return json { { "redone_label", label } };
        }, true));

    r.add (makeProjectCommand ("history.list", obj ({}), obj ({}), false,
        [] (Project& p, const json&, const CommandContext&) -> Result<json>
        {
            json undo = json::array(), redo = json::array();

            for (auto& s : p.undoSteps())   undo.push_back (s.toJson());
            for (auto& s : p.redoSteps())   redo.push_back (s.toJson());

            return json { { "undo", undo }, { "redo", redo } };
        }));
}
} // namespace

void registerImportCommands (CommandRegistry&);
void registerEditCommands (CommandRegistry&);
void registerTransportCommands (CommandRegistry&);
void registerPluginCommands (CommandRegistry&);
void registerMidiCommands (CommandRegistry&);
void registerRoutingCommands (CommandRegistry&);
void registerTemplateCommands (CommandRegistry&);

void registerBuiltinCommands (CommandRegistry& r)
{
    registerProjectCommands (r);
    registerTrackCommands (r);
    registerFxCommands (r);
    registerMarkerCommands (r);
    registerHistoryCommands (r);
    registerImportCommands (r);
    registerEditCommands (r);
    registerTransportCommands (r);
    registerPluginCommands (r);
    registerMidiCommands (r);
    registerRoutingCommands (r);
    registerTemplateCommands (r);
}

} // namespace rovox
