// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// M4 commands (tasks 400, 410, 420): plugin.catalog / rescan / probe / tag / unblacklist, fx.presets / preset_load / preset_save / move.
#include "Command.h"

#include "PluginCatalog.h"
#include "ParamsAction.h"
#include "PluginHost.h"
#include "ProjectManager.h"
#include "Sha1.h"

#include <fstream>
#include <map>

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

te::ExternalPlugin* externalPlugin (Project& p, const std::string& id, Error& error)
{
    auto* plugin = p.findPlugin (id);

    if (plugin == nullptr)
    {
        error = makeError (errc::notFound, "no plugin " + id);
        return nullptr;
    }

    auto* ext = dynamic_cast<te::ExternalPlugin*> (plugin);

    if (ext == nullptr)
        error = makeError (errc::invalidParams, "plugin " + id + " is a built-in effect: it has no presets (use fx.update)");
    else if (ext->getAudioPluginInstance() == nullptr)
        error = makeError (errc::pluginLoadFailed, "plugin " + id + " did not load: " + toStd (ext->getLoadError()));

    return error.message.empty() ? ext : nullptr;
}

fs::path userPresetFile (EngineHost& host, const std::string& uid, const std::string& name)
{
    std::string safe;

    for (char c : name)
        safe += (std::isalnum ((unsigned char) c) || c == '-' || c == '_' || c == ' ') ? c : '_';

    return host.catalog().userPresetDir (uid) / (safe + ".rvxpreset");
}

json presetsOfLive (Project& p, te::ExternalPlugin& ext, std::string& uidOut)
{
    uidOut = pluginUidFor (ext.desc);
    auto* inst = ext.getAudioPluginInstance();
    json presets = json::array();

    for (int i = 0; i < inst->getNumPrograms(); ++i)
    {
        const auto name = toStd (inst->getProgramName (i));
        presets.push_back ({ { "preset_id", PluginCatalog::presetId (uidOut, name) }, { "name", name }, { "source", "factory" }, { "program_index", i }, { "tags", json::array() } });
    }

    std::error_code ec;

    for (auto& f : fs::directory_iterator (p.host().catalog().userPresetDir (uidOut), ec))
    {
        if (f.path().extension() != ".rvxpreset")
            continue;

        std::ifstream in (f.path());
        auto doc = json::parse (in, nullptr, false);

        if (! doc.is_discarded())
            presets.push_back ({ { "preset_id", PluginCatalog::presetId (uidOut, doc.value ("name", "")) }, { "name", doc.value ("name", "") }, { "source", "user" },
                                 { "tags", doc.value ("tags", json::array()) } });
    }

    return presets;
}
} // namespace

void registerPluginCommands (CommandRegistry& r)
{
    r.add (makeGlobalCommand ("plugin.catalog",
        obj ({ { "kind", { { "type", "string" }, { "enum", { "effect", "instrument" } } } }, { "query", str() }, { "format", str() }, { "tag", str() },
               { "status", { { "type", "string" }, { "enum", { "ok", "blacklisted" } } } }, { "include_presets", boolean() },
               { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 } } }, { "cursor", str() } }),
        anyObject(), false,
        [] (ProjectManager& pm, const json& params, const CommandContext&) -> Result<json>
        {
            auto& catalog = pm.host().catalog();

            if (catalog.size() == 0 && catalog.scanState().value ("state", "") == "never")
                catalog.rescan();           // first use without a startup scan: scan now

            CatalogQuery q;
            q.kind = params.value ("kind", "");
            q.query = params.value ("query", "");
            q.format = params.value ("format", "");
            q.tag = params.value ("tag", "");
            q.status = params.value ("status", "");
            q.includePresets = params.value ("include_presets", false);
            q.limit = (size_t) params.value ("limit", 50);
            q.offset = params.contains ("cursor") ? (size_t) std::atoll (params["cursor"].get<std::string>().c_str()) : 0;
            return catalog.query (q);
        }));

    r.add (makeGlobalCommand ("plugin.rescan", obj ({ { "force", boolean() } }), anyObject(), false,
        [] (ProjectManager& pm, const json& params, const CommandContext&) -> Result<json>
        {
            return pm.host().catalog().rescan (params.value ("force", false));
        }));

    r.add (makeGlobalCommand ("plugin.probe", obj ({ { "plugin_uid", strMin (1) } }, { "plugin_uid" }), anyObject(), false,
        [] (ProjectManager& pm, const json& params, const CommandContext&) -> Result<json>
        {
            auto info = pm.host().catalog().ensureProbed (params["plugin_uid"].get<std::string>());

            if (! info.ok())
                return info.error();

            return json { { "plugin_uid", params["plugin_uid"] }, { "probe", info.value() } };
        }));

    r.add (makeGlobalCommand ("plugin.tag", obj ({ { "plugin_uid", strMin (1) }, { "tags", arrayOf (str()) } }, { "plugin_uid", "tags" }), anyObject(), true,
        [] (ProjectManager& pm, const json& params, const CommandContext&) -> Result<json>
        {
            return pm.host().catalog().setTags (params["plugin_uid"].get<std::string>(), params["tags"].get<std::vector<std::string>>());
        }));

    r.add (makeGlobalCommand ("plugin.unblacklist", obj ({ { "plugin_uid", strMin (1) } }, { "plugin_uid" }), anyObject(), true,
        [] (ProjectManager& pm, const json& params, const CommandContext&) -> Result<json>
        {
            pm.host().catalog().unblacklist (params["plugin_uid"].get<std::string>());
            return json { { "plugin_uid", params["plugin_uid"] } };
        }));

    //==========================================================================
    r.add (makeProjectCommand ("fx.presets", obj ({ { "plugin_id", strMin (1) } }, { "plugin_id" }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            Error error;
            auto* ext = externalPlugin (p, params["plugin_id"].get<std::string>(), error);

            if (ext == nullptr)
                return error;

            std::string uid;
            auto presets = presetsOfLive (p, *ext, uid);
            return json { { "plugin_id", params["plugin_id"] }, { "plugin_uid", uid }, { "presets", presets },
                          { "current_program", ext->getAudioPluginInstance()->getNumPrograms() > 0 ? json (ext->getAudioPluginInstance()->getCurrentProgram()) : json (nullptr) } };
        }));

    r.add (makeProjectCommand ("fx.preset_load", obj ({ { "plugin_id", strMin (1) }, { "preset_id", strMin (1) } }, { "plugin_id", "preset_id" }), anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            Error error;
            auto* ext = externalPlugin (p, params["plugin_id"].get<std::string>(), error);

            if (ext == nullptr)
                return error;

            std::string uid;
            const auto presets = presetsOfLive (p, *ext, uid);
            const json* found = nullptr;

            for (auto& pr : presets)
                if (pr["preset_id"] == params["preset_id"])
                    found = &pr;

            if (found == nullptr)
                return makeError (errc::notFound, "no preset " + params["preset_id"].get<std::string>() + " for this plugin (fx.presets lists them)");

            auto* inst = ext->getAudioPluginInstance();

            if ((*found)["source"] == "factory")
            {
                inst->setCurrentProgram ((*found)["program_index"].get<int>());
            }
            else
            {
                std::ifstream in (userPresetFile (p.host(), uid, (*found)["name"].get<std::string>()));
                auto doc = json::parse (in, nullptr, false);
                juce::MemoryBlock block;

                if (doc.is_discarded())
                    return makeError (errc::io, "the user preset file is unreadable");

                if (block.fromBase64Encoding (toJuce (doc.value ("state", ""))) && block.getSize() > 0)
                    inst->setStateInformation (block.getData(), (int) block.getSize());

                // plugins whose state does not include their control values (most LV2 effects): the values are saved next to it
                if (doc.contains ("params") && doc["params"].is_object())
                {
                    std::map<std::string, float> before, after;

                    for (auto* ap : ext->getAutomatableParameters())
                        if (auto v = doc["params"].find (toStd (ap->paramID)); v != doc["params"].end() && v->is_number())
                        {
                            before[toStd (ap->paramID)] = ap->getCurrentExplicitValue();
                            after[toStd (ap->paramID)] = v->get<float>();
                        }

                    p.edit().getUndoManager().perform (new SetParamsAction (ext, std::move (before), std::move (after)));
                }
            }

            ext->flushPluginStateToValueTree();         // into the undoable tree, so that undo, save and renders see the preset
            return json { { "plugin_id", params["plugin_id"] }, { "preset", *found } };
        }));

    r.add (makeProjectCommand ("fx.preset_save", obj ({ { "plugin_id", strMin (1) }, { "name", strMin (1) }, { "tags", arrayOf (str()) } }, { "plugin_id", "name" }), anyObject(), false,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            Error error;
            auto* ext = externalPlugin (p, params["plugin_id"].get<std::string>(), error);

            if (ext == nullptr)
                return error;

            const auto uid = pluginUidFor (ext->desc);
            juce::MemoryBlock block;
            ext->getAudioPluginInstance()->getStateInformation (block);
            const auto name = params["name"].get<std::string>();
            const auto file = userPresetFile (p.host(), uid, name);
            std::error_code ec;
            fs::create_directories (file.parent_path(), ec);
            json values = json::object();

            for (auto* ap : ext->getAutomatableParameters())
                values[toStd (ap->paramID)] = (double) ap->getCurrentExplicitValue();

            const json doc { { "plugin_uid", uid }, { "name", name }, { "tags", params.value ("tags", json::array()) }, { "state", toStd (block.toBase64Encoding()) }, { "params", values } };

            if (auto w = writeFileAtomic (file, doc.dump(), false); ! w.ok())
                return w.error();

            return json { { "preset_id", PluginCatalog::presetId (uid, name) }, { "name", name }, { "source", "user" } };
        }));

    r.add (makeProjectCommand ("fx.move", obj ({ { "plugin_id", strMin (1) }, { "to_index", { { "type", "integer" }, { "minimum", 0 } } } }, { "plugin_id", "to_index" }), anyObject(), true,
        [] (Project& p, const json& params, const CommandContext&) -> Result<json>
        {
            auto* plugin = p.findPlugin (params["plugin_id"].get<std::string>());

            if (plugin == nullptr)
                return makeError (errc::notFound, "no plugin " + params["plugin_id"].get<std::string>());

            auto* list = plugin->getOwnerList();

            if (list == nullptr)
                return makeError (errc::preconditionFailed, "the plugin is not in a chain");

            const int count = list->size();
            const int to = std::min (params["to_index"].get<int>(), count - 1);
            auto* target = (*list)[to];
            const int from = list->indexOf (plugin);

            if (from != to)
            {
                auto parent = plugin->state.getParent();
                parent.moveChild (parent.indexOf (plugin->state), parent.indexOf (target->state), &p.edit().getUndoManager());
            }

            json chain = json::array();

            for (auto* pl : list->getPlugins())
                chain.push_back (Project::idOf (pl->state));

            return json { { "plugin_id", params["plugin_id"] }, { "index", list->indexOf (plugin) }, { "chain", chain } };
        }));
}

} // namespace rovox
