// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Project templates (task 660, MVP): template.list / get / save / apply / delete. A template is a JSON document (tracks with instruments,
// effects, sends, master chain) under <data>/templates/<id>/template.json plus a few built-in ones. Applying it runs the ordinary
// commands (track.create, fx.add, routing.send_create, item.create_midi) inside one undo step, so every rule of those commands holds.
#include "Command.h"

#include "PluginHost.h"
#include "ProjectFormat.h"
#include "ProjectManager.h"

#include <algorithm>
#include <fstream>
#include <map>
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
json arrayOf (json items)           { return { { "type", "array" }, { "items", std::move (items) } }; }

std::string slug (const std::string& s)
{
    std::string out;

    for (char c : s)
    {
        if (std::isalnum ((unsigned char) c))   out += (char) std::tolower ((unsigned char) c);
        else if (! out.empty() && out.back() != '-') out += '-';
    }

    while (! out.empty() && out.back() == '-')
        out.pop_back();

    return out.empty() ? "track" : out;
}

bool validTemplateId (const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;

    return std::all_of (id.begin(), id.end(), [] (unsigned char c) { return std::isalnum (c) || c == '-' || c == '_'; });
}

const char* builtinNames[] = { "equaliser", "compressor", "reverb", "delay", "chorus", "phaser", "lowpass", "fouroscillator" };

std::string builtinTypeOf (const std::string& name)
{
    if (name == "equaliser")      return te::EqualiserPlugin::xmlTypeName;
    if (name == "compressor")     return te::CompressorPlugin::xmlTypeName;
    if (name == "reverb")         return te::ReverbPlugin::xmlTypeName;
    if (name == "phaser")         return te::PhaserPlugin::xmlTypeName;
    if (name == "lowpass")        return te::LowPassPlugin::xmlTypeName;
    if (name == "delay")          return te::DelayPlugin::xmlTypeName;
    if (name == "chorus")         return te::ChorusPlugin::xmlTypeName;
    if (name == "fouroscillator") return te::FourOscPlugin::xmlTypeName;
    return {};
}

std::string builtinNameOf (const std::string& type)
{
    for (auto* n : builtinNames)
        if (builtinTypeOf (n) == type)
            return n;

    return {};
}

fs::path templatesDir (EngineHost& host, const std::string& tenant)
{
    return tenant == "local" ? host.dataDir() / "templates" : host.dataDir() / "tenants" / tenant / "templates";
}

json builtinTemplates()
{
    return json::array ({
        { { "id", "audio-band" }, { "name", "Audio band" }, { "version", 1 }, { "builtin", true },
          { "description", "Vocal, guitar, bass and drums tracks with a reverb bus and a gentle compressor on the master. Needs no plugins." },
          { "tags", { "mix", "audio" } }, { "tempo", { { "bpm", 100.0 } } },
          { "tracks", json::array ({
              { { "role", "vocal" }, { "name", "Vocal" }, { "volume_db", -3.0 }, { "pan", 0.0 }, { "color", "#ff7a59" }, { "fx", json::array ({ { { "builtin", "compressor" } } }) },
                { "sends", json::array ({ { { "to_role", "reverb" }, { "gain_db", -12.0 } } }) } },
              { { "role", "guitar" }, { "name", "Guitar" }, { "volume_db", -6.0 }, { "pan", -0.3 }, { "color", "#59c3ff" }, { "fx", json::array() },
                { "sends", json::array ({ { { "to_role", "reverb" }, { "gain_db", -18.0 } } }) } },
              { { "role", "bass" }, { "name", "Bass" }, { "volume_db", -6.0 }, { "pan", 0.0 }, { "color", "#7ad97a" }, { "fx", json::array ({ { { "builtin", "compressor" } } }) }, { "sends", json::array() } },
              { { "role", "drums" }, { "name", "Drums" }, { "volume_db", -5.0 }, { "pan", 0.0 }, { "color", "#e0c341" }, { "fx", json::array() }, { "sends", json::array() } },
              { { "role", "reverb" }, { "name", "Reverb" }, { "volume_db", 0.0 }, { "pan", 0.0 }, { "color", "#b08cff" }, { "fx", json::array ({ { { "builtin", "reverb" } } }) }, { "sends", json::array() } } }) },
          { "master", { { "volume_db", 0.0 }, { "fx", json::array ({ { { "builtin", "compressor" } } }) } } } },

        { { "id", "drums-bass-keys" }, { "name", "Drums, bass and keys (LV2 instruments)" }, { "version", 1 }, { "builtin", true },
          { "description", "MIDI tracks played by installed instruments: a drum kit, a bass synth and a keys synth, with a reverb bus. Instruments are looked up in the plugin catalog." },
          { "tags", { "midi", "instruments" } },
          { "tracks", json::array ({
              { { "role", "drums" }, { "name", "Drums" }, { "volume_db", -4.0 }, { "pan", 0.0 }, { "instrument", { { "kind", "instrument" }, { "query", "drum" } } }, { "fx", json::array() },
                { "sends", json::array ({ { { "to_role", "reverb" }, { "gain_db", -16.0 } } }) } },
              { { "role", "bass" }, { "name", "Bass" }, { "volume_db", -6.0 }, { "pan", 0.0 }, { "instrument", { { "kind", "instrument" }, { "query", "zynaddsubfx" } } }, { "fx", json::array() }, { "sends", json::array() } },
              { { "role", "keys" }, { "name", "Keys" }, { "volume_db", -8.0 }, { "pan", 0.2 }, { "instrument", { { "kind", "instrument" }, { "query", "organ" } } }, { "fx", json::array() },
                { "sends", json::array ({ { { "to_role", "reverb" }, { "gain_db", -10.0 } } }) } },
              { { "role", "reverb" }, { "name", "Reverb" }, { "volume_db", 0.0 }, { "pan", 0.0 }, { "fx", json::array ({ { { "builtin", "reverb" } } }) }, { "sends", json::array() } } }) },
          { "master", { { "volume_db", 0.0 }, { "fx", json::array() } } } } });
}

std::optional<json> loadTemplate (EngineHost& host, const std::string& tenant, const std::string& id)
{
    for (auto& t : builtinTemplates())
        if (t["id"] == id)
            return t;

    std::ifstream in (templatesDir (host, tenant) / id / "template.json");

    if (! in)
        return std::nullopt;

    auto doc = json::parse (in, nullptr, false);

    if (doc.is_discarded() || ! doc.is_object())
        return std::nullopt;

    return doc;
}

//==============================================================================
json capturePlugin (te::Plugin& pl)
{
    json j { { "enabled", pl.isEnabled() } };

    if (auto* ext = dynamic_cast<te::ExternalPlugin*> (&pl))
    {
        j["plugin"] = { { "plugin_uid", pluginUidFor (ext->desc) }, { "name", toStd (ext->desc.name) }, { "format", toStd (ext->desc.pluginFormatName) } };

        if (ext->state.hasProperty (te::IDs::state))
            j["state"] = ext->state[te::IDs::state].toString().toStdString();

        return j;
    }

    const auto name = builtinNameOf (toStd (pl.getPluginType()));

    if (name.empty())
        return nullptr;

    json params = json::object();

    for (auto* ap : pl.getAutomatableParameters())
        params[toStd (ap->paramID)] = (double) ap->getCurrentExplicitValue();

    j["builtin"] = name;
    j["params"] = params;
    return j;
}

json captureTemplate (Project& p)
{
    json tracks = json::array();
    std::map<int, std::string> roleOfBus;
    std::set<std::string> usedRoles;
    std::map<te::AudioTrack*, std::string> roles;

    for (auto* t : te::getAudioTracks (p.edit()))
    {
        auto role = slug (toStd (t->getName()));

        for (int n = 2; usedRoles.count (role) > 0; ++n)
            role = slug (toStd (t->getName())) + "-" + std::to_string (n);

        usedRoles.insert (role);
        roles[t] = role;

        if (auto* ret = t->pluginList.findFirstPluginOfType<te::AuxReturnPlugin>())
            roleOfBus[ret->busNumber.get()] = role;
    }

    for (auto* t : te::getAudioTracks (p.edit()))
    {
        json fx = json::array(), sends = json::array();
        json instrument = nullptr;

        for (auto* pl : t->pluginList.getPlugins())
        {
            if (dynamic_cast<te::VolumeAndPanPlugin*> (pl) != nullptr || dynamic_cast<te::AuxReturnPlugin*> (pl) != nullptr)
                continue;

            if (auto* s = dynamic_cast<te::AuxSendPlugin*> (pl))
            {
                sends.push_back ({ { "to_role", roleOfBus.count (s->busNumber.get()) ? roleOfBus[s->busNumber.get()] : std::string() }, { "gain_db", (double) s->getGainDb() },
                                   { "pre_fader", t->getVolumePlugin() != nullptr && t->pluginList.indexOf (pl) < t->pluginList.indexOf (t->getVolumePlugin()) } });
                continue;
            }

            auto c = capturePlugin (*pl);

            if (c.is_null())
                continue;

            if (pl->isSynth() && instrument.is_null())
                instrument = c;
            else
                fx.push_back (c);
        }

        auto* vp = t->getVolumePlugin();
        json tj { { "role", roles[t] }, { "name", toStd (t->getName()) }, { "volume_db", vp != nullptr ? (double) te::volumeFaderPositionToDB (vp->volume.get()) : 0.0 },
                  { "pan", vp != nullptr ? (double) vp->pan.get() : 0.0 }, { "color", "#" + t->getColour().toDisplayString (false).toLowerCase().toStdString() },
                  { "fx", fx }, { "sends", sends } };

        if (! instrument.is_null())
            tj["instrument"] = instrument;

        tracks.push_back (tj);
    }

    json masterFx = json::array();

    for (auto* pl : p.edit().getMasterPluginList().getPlugins())
        if (dynamic_cast<te::VolumeAndPanPlugin*> (pl) == nullptr)
            if (auto c = capturePlugin (*pl); ! c.is_null())
                masterFx.push_back (c);

    auto vp = p.edit().getMasterVolumePlugin();
    json master { { "volume_db", vp != nullptr ? (double) te::volumeFaderPositionToDB (vp->volume.get()) : 0.0 }, { "fx", masterFx } };
    return { { "tracks", tracks }, { "master", master }, { "tempo", { { "bpm", p.edit().tempoSequence.getTempo (0)->getBpm() } } } };
}

/** Plugins a template needs from the catalog, by track: nullopt entries are builtin. */
struct Dependency { std::string where; json spec; };

void collectDependencies (const json& entry, const std::string& where, std::vector<Dependency>& out)
{
    if (entry.is_object() && (entry.contains ("plugin") || entry.contains ("query") || entry.contains ("plugin_uid")))
        out.push_back ({ where, entry });
}

std::optional<json> resolveInCatalog (PluginCatalog& catalog, const json& spec, std::string& problem)
{
    std::optional<json> rec;

    if (spec.contains ("plugin") && spec["plugin"].contains ("plugin_uid"))
        rec = catalog.find (spec["plugin"]["plugin_uid"].get<std::string>());
    else if (spec.contains ("plugin_uid"))
        rec = catalog.find (spec["plugin_uid"].get<std::string>());
    else if (spec.contains ("query"))
    {
        CatalogQuery q;
        q.kind = spec.value ("kind", "");
        q.query = spec["query"].get<std::string>();
        q.status = "ok";
        q.limit = 1;
        auto res = catalog.query (q);

        if (! res["plugins"].empty())
            rec = catalog.find (res["plugins"][0].value ("plugin_uid", ""));
    }

    if (! rec)
        problem = "not installed";
    else if (auto why = catalog.blacklistReason (rec->value ("plugin_uid", "")))
        problem = "blacklisted: " + *why;

    return problem.empty() ? rec : std::nullopt;
}
} // namespace

void registerTemplateCommands (CommandRegistry& r)
{
    r.add (makeGlobalCommand ("template.list", obj ({ { "tag", str() } }), anyObject(), false,
        [] (ProjectManager& pm, const json& params, const CommandContext& ctx) -> Result<json>
        {
            json out = json::array();
            const auto tag = params.value ("tag", "");

            auto add = [&] (const json& t)
            {
                const json tags = t.value ("tags", json::array());

                if (! tag.empty() && std::find (tags.begin(), tags.end(), tag) == tags.end())
                    return;

                out.push_back ({ { "template_id", t["id"] }, { "name", t.value ("name", "") }, { "version", t.value ("version", 1) }, { "description", t.value ("description", "") },
                                 { "tags", t.value ("tags", json::array()) }, { "builtin", t.value ("builtin", false) }, { "tracks", t.value ("tracks", json::array()).size() } });
            };

            for (auto& t : builtinTemplates())
                add (t);

            std::error_code ec;

            for (auto& e : fs::directory_iterator (templatesDir (pm.host(), ctx.tenant), ec))
                if (auto t = loadTemplate (pm.host(), ctx.tenant, e.path().filename().string()))
                    add (*t);

            return json { { "templates", out } };
        }));

    r.add (makeGlobalCommand ("template.get", obj ({ { "template_id", strMin (1) } }, { "template_id" }), anyObject(), false,
        [] (ProjectManager& pm, const json& params, const CommandContext& ctx) -> Result<json>
        {
            auto t = loadTemplate (pm.host(), ctx.tenant, params["template_id"].get<std::string>());

            if (! t)
                return makeError (errc::notFound, "no template " + params["template_id"].get<std::string>());

            return json { { "template", *t } };
        }));

    r.add (makeGlobalCommand ("template.delete", obj ({ { "template_id", strMin (1) } }, { "template_id" }), anyObject(), true,
        [] (ProjectManager& pm, const json& params, const CommandContext& ctx) -> Result<json>
        {
            const auto id = params["template_id"].get<std::string>();

            if (! validTemplateId (id))
                return makeError (errc::invalidParams, "bad template id", json { { "path", "/template_id" } });

            for (auto& t : builtinTemplates())
                if (t["id"] == id)
                    return makeError (errc::preconditionFailed, "built-in templates cannot be deleted");

            std::error_code ec;

            if (fs::remove_all (templatesDir (pm.host(), ctx.tenant) / id, ec) == 0)
                return makeError (errc::notFound, "no template " + id);

            return json { { "template_id", id } };
        }));

    r.add (makeProjectCommand ("template.save",
        obj ({ { "template_id", strMin (1) }, { "name", strMin (1) }, { "description", str() }, { "tags", arrayOf (str()) } }, { "template_id" }),
        anyObject(), false,
        [] (Project& p, const json& params, const CommandContext& ctx) -> Result<json>
        {
            const auto id = params["template_id"].get<std::string>();

            if (! validTemplateId (id))
                return makeError (errc::invalidParams, "a template id is letters, digits, - and _", json { { "path", "/template_id" } });

            for (auto& t : builtinTemplates())
                if (t["id"] == id)
                    return makeError (errc::preconditionFailed, "that id belongs to a built-in template");

            auto doc = captureTemplate (p);
            doc["id"] = id;
            doc["name"] = params.value ("name", id);
            doc["description"] = params.value ("description", "");
            doc["tags"] = params.value ("tags", json::array());
            doc["version"] = 1;

            if (auto previous = loadTemplate (p.host(), ctx.tenant, id))
                doc["version"] = previous->value ("version", 1) + 1;

            doc["saved_from"] = p.id();
            doc["saved_at"] = nowIso8601();
            const auto dir = templatesDir (p.host(), ctx.tenant) / id;
            std::error_code ec;
            fs::create_directories (dir, ec);

            if (auto w = writeFileAtomic (dir / "template.json", doc.dump (2), false); ! w.ok())
                return w.error();

            return json { { "template_id", id }, { "version", doc["version"] }, { "tracks", doc["tracks"].size() }, { "template", doc } };
        }));

    r.add (makeProjectCommand ("template.apply", obj ({ { "template_id", strMin (1) }, { "bindings", anyObject() } }, { "template_id" }), anyObject(), true,
        [&r] (Project& p, const json& params, const CommandContext& ctx) -> Result<json>
        {
            auto t = loadTemplate (p.host(), ctx.tenant, params["template_id"].get<std::string>());

            if (! t)
                return makeError (errc::notFound, "no template " + params["template_id"].get<std::string>());

            auto& catalog = p.host().catalog();

            if (catalog.size() == 0)
                catalog.rescan();

            // 1. everything the template needs from the plugin catalog must be there before anything is created
            std::vector<Dependency> deps;

            for (auto& tr : (*t)["tracks"])
            {
                const auto where = "track " + tr.value ("role", "?");

                if (tr.contains ("instrument"))
                    collectDependencies (tr["instrument"], where + " instrument", deps);

                for (auto& f : tr.value ("fx", json::array()))
                    collectDependencies (f, where + " fx", deps);
            }

            for (auto& f : (*t)["master"].value ("fx", json::array()))
                collectDependencies (f, "master fx", deps);

            json missing = json::array();

            for (auto& d : deps)
            {
                std::string problem;
                auto rec = resolveInCatalog (catalog, d.spec, problem);

                if (! rec)
                    missing.push_back ({ { "where", d.where }, { "needs", d.spec.contains ("plugin") ? d.spec["plugin"] : d.spec }, { "reason", problem } });
            }

            if (! missing.empty())
                return makeError (errc::preconditionFailed, "the template needs plugins that are not available on this server", json { { "missing", missing } });

            const json bindings = params.value ("bindings", json::object());
            std::set<std::string> roles;

            for (auto& tr : (*t)["tracks"])
                roles.insert (tr.value ("role", ""));

            for (auto& [role, binding] : bindings.items())
                if (roles.count (role) == 0)
                    return makeError (errc::invalidParams, "the template has no track with role '" + role + "'", json { { "path", "/bindings/" + role } });

            auto run = [&] (const char* command, const json& args) -> Result<json>
            {
                return r.find (command)->execute (p, args, ctx);
            };

            auto applyFx = [&] (const std::string& target, const json& f, json& added) -> std::optional<Error>
            {
                json args { { "track_id", target } };

                if (f.contains ("builtin"))
                {
                    args["builtin"] = f["builtin"];
                }
                else
                {
                    std::string problem;
                    auto rec = resolveInCatalog (catalog, f, problem);
                    args["plugin"] = { { "plugin_uid", rec->value ("plugin_uid", "") } };
                }

                auto res = run ("fx.add", args);

                if (! res.ok())
                    return res.error();

                const auto pluginId = res.value()["plugin_id"].get<std::string>();
                auto* pl = p.findPlugin (pluginId);

                if (pl != nullptr)
                {
                    if (f.contains ("params"))
                        for (auto& [ident, value] : f["params"].items())
                            for (auto* ap : pl->getAutomatableParameters())
                                if (toStd (ap->paramID) == ident && value.is_number())
                                    ap->setParameter (value.get<float>(), juce::sendNotification);

                    if (f.contains ("state") && f["state"].is_string())
                        if (auto* ext = dynamic_cast<te::ExternalPlugin*> (pl); ext != nullptr && ext->getAudioPluginInstance() != nullptr)
                        {
                            juce::MemoryBlock block;

                            if (block.fromBase64Encoding (toJuce (f["state"].get<std::string>())) && block.getSize() > 0)
                            {
                                ext->getAudioPluginInstance()->setStateInformation (block.getData(), (int) block.getSize());
                                ext->flushPluginStateToValueTree();
                            }
                        }

                    if (! f.value ("enabled", true))
                        pl->setEnabled (false);
                }

                added.push_back (pluginId);
                return std::nullopt;
            };

            // 2. tracks, instruments and effects
            json created = json::array();
            std::map<std::string, std::string> trackOfRole;

            for (auto& tr : (*t)["tracks"])
            {
                json args { { "name", tr.value ("name", "Track") }, { "volume_db", tr.value ("volume_db", 0.0) }, { "pan", tr.value ("pan", 0.0) } };

                if (tr.contains ("color") && tr["color"].is_string())
                    args["color"] = tr["color"];

                auto track = run ("track.create", args);

                if (! track.ok())
                    return track.error();

                const auto trackId = track.value()["track_id"].get<std::string>();
                trackOfRole[tr.value ("role", "")] = trackId;
                json plugins = json::array();

                if (tr.contains ("instrument"))
                    if (auto e = applyFx (trackId, tr["instrument"], plugins))
                        return *e;

                for (auto& f : tr.value ("fx", json::array()))
                    if (auto e = applyFx (trackId, f, plugins))
                        return *e;

                created.push_back ({ { "role", tr.value ("role", "") }, { "track_id", trackId }, { "plugins", plugins } });
            }

            // 3. sends (all tracks exist now)
            for (auto& tr : (*t)["tracks"])
                for (auto& s : tr.value ("sends", json::array()))
                {
                    const auto to = s.value ("to_role", "");

                    if (trackOfRole.count (to) == 0)
                        continue;

                    auto send = run ("routing.send_create", { { "track_id", trackOfRole[tr.value ("role", "")] }, { "to_track_id", trackOfRole[to] }, { "gain_db", s.value ("gain_db", 0.0) },
                                                              { "pre_fader", s.value ("pre_fader", false) } });

                    if (! send.ok())
                        return send.error();
                }

            // 4. master and tempo
            json masterPlugins = json::array();

            if ((*t).contains ("master"))
            {
                if ((*t)["master"].contains ("volume_db"))
                    if (auto m = run ("track.update", { { "track_id", "master" }, { "volume_db", (*t)["master"]["volume_db"] } }); ! m.ok())
                        return m.error();

                for (auto& f : (*t)["master"].value ("fx", json::array()))
                    if (auto e = applyFx ("master", f, masterPlugins))
                        return *e;
            }

            if ((*t).contains ("tempo") && (*t)["tempo"].contains ("bpm"))
                if (auto* tempo = p.edit().tempoSequence.getTempo (0))
                    tempo->setBpm ((*t)["tempo"]["bpm"].get<double>());

            // 5. what the caller supplied for the tracks: MIDI notes or a MIDI file
            json items = json::array();

            for (auto& [role, binding] : bindings.items())
            {
                json args { { "track_id", trackOfRole[role] } };
                const char* command = "item.create_midi";

                if (binding.contains ("midi_file"))
                {
                    command = "midi.import_file";
                    args["file"] = binding["midi_file"];
                }
                else if (binding.contains ("notes"))
                {
                    args["notes"] = binding["notes"];
                }
                else
                {
                    return makeError (errc::invalidParams, "a binding is {midi_file} or {notes}", json { { "path", "/bindings/" + role } });
                }

                if (binding.contains ("start"))     args["start"] = binding["start"];
                if (binding.contains ("start_s"))   args["start_s"] = binding["start_s"];

                auto item = run (command, args);

                if (! item.ok())
                    return item.error();

                items.push_back ({ { "role", role }, { "item_id", item.value()["item_id"] } });
            }

            static const juce::Identifier templateId ("rovox:template");
            p.edit().state.setProperty (templateId, toJuce ((*t)["id"].get<std::string>() + "@" + std::to_string ((*t).value ("version", 1))), &p.edit().getUndoManager());

            return json { { "template", { { "id", (*t)["id"] }, { "version", (*t).value ("version", 1) } } }, { "tracks", created }, { "master_plugins", masterPlugins }, { "items", items } };
        }));
}

} // namespace rovox
