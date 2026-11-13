// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Scopes.h"

#include <map>

namespace rovox
{

namespace
{
using S = Scope;

const std::map<std::string, std::vector<Scope>>& table()
{
    static const std::map<std::string, std::vector<Scope>> t = []
    {
        std::map<std::string, std::vector<Scope>> m;

        auto add = [&] (std::initializer_list<const char*> names, std::vector<Scope> scopes)
        {
            for (auto* n : names)
                m[n] = scopes;
        };

        add ({ "plugin.catalog", "plugin.probe", "template.list", "template.get", "recipe.list", "recipe.get", "delivery.list_adapters" }, { S::catalogRead });

        add ({ "project.list", "project.catalog", "project.info", "project.get", "project.activity", "project.diff", "history.list", "track.list", "item.list", "item.source",
               "fx.list", "fx.params", "fx.presets", "marker.list", "midi.get", "routing.list", "tempo.get", "transport.get", "mixer.get_meters", "time.convert",
               "job.get", "job.list", "events.poll", "events.subscribe", "upload.status", "delivery.list", "review.wait", "review.list", "usage.summary" }, { S::projectsRead });
        add ({ "review.resolve" }, { S::projectsWrite });
        add ({ "review.request" }, { S::projectsWrite, S::delivery });

        add ({ "project.new", "project.open", "project.close", "project.save", "project.update", "project.set_ttl", "project.delete", "project.mix",
               "track.create", "track.delete", "track.duplicate", "track.move", "track.update",
               "item.create_midi", "item.delete", "item.duplicate", "item.split", "item.update",
               "fx.add", "fx.move", "fx.remove", "fx.update", "fx.preset_load", "fx.preset_save",
               "marker.create", "marker.update", "marker.upsert", "marker.delete",
               "midi.write", "midi.edit", "midi.quantize", "midi.transpose", "midi.import_file", "midi.export_file",
               "routing.send_create", "routing.send_update", "routing.send_delete", "tempo.set", "transport.set", "undo", "redo", "agent.accept",
               "import.plan", "import.apply", "import.rpp", "upload.from_url", "template.apply", "template.new", "template.save", "template.delete", "recipe.save", "recipe.delete" }, { S::projectsWrite });

        add ({ "render.preflight", "render.start", "render.batch", "render.preview", "analyze.audio" }, { S::render });
        add ({ "recipe.run" }, { S::render, S::projectsWrite });
        add ({ "delivery.send", "delivery.release" }, { S::delivery });
        add ({ "plugin.rescan", "plugin.tag", "plugin.unblacklist", "usage.report" }, { S::admin });

        // checked per operation (batch) or per job kind (job.cancel) by the caller
        add ({ "batch", "job.cancel" }, {});

        // the old mcp__reaper__ bridge names (reaperCompat): same scope as the tool they map to
        add ({ "list_projects", "get_project", "get_undo_history", "list_tracks", "list_items", "list_markers", "get_tempo_map", "time_convert", "list_fx", "get_fx_params", "get_render_job" }, { S::projectsRead });
        add ({ "new_project", "open_project", "close_project", "save_project", "create_tracks", "update_tracks", "delete_tracks", "split_items", "delete_items", "update_items",
               "upsert_markers", "delete_markers", "set_tempo_map", "add_fx", "remove_fx", "update_fx" }, { S::projectsWrite });
        add ({ "render" }, { S::render });
        add ({ "cancel_render" }, {});      // job.cancel: checked by the kind of the job

        return m;
    }();

    return t;
}

bool isAdminPrefix (const std::string& name)
{
    return name.rfind ("key.", 0) == 0 || name.rfind ("tenant.", 0) == 0;
}
} // namespace

bool hasScopeEntry (const std::string& name)
{
    return table().count (name) > 0 || isAdminPrefix (name);
}

std::vector<Scope> requiredScopes (const std::string& name)
{
    if (auto it = table().find (name); it != table().end())
        return it->second;

    return { S::admin };
}

std::optional<Scope> missingScope (const Principal& p, const std::string& name)
{
    for (auto s : requiredScopes (name))
        if (! p.has (s))
            return s;

    return std::nullopt;
}

bool allowed (const Principal& p, const std::string& name)
{
    return ! missingScope (p, name).has_value();
}

Scope scopeForJobKind (const std::string& kind)
{
    if (kind == "delivery")
        return Scope::delivery;

    if (kind == "import_plan" || kind == "upload_fetch")
        return Scope::projectsWrite;

    return Scope::render;
}

Error forbiddenScopeError (Scope missing, const std::string& name)
{
    return makeError (errc::forbiddenScope, std::string ("the scope ") + scopeName (missing) + " is required for " + name, json { { "scope", scopeName (missing) } });
}

} // namespace rovox
