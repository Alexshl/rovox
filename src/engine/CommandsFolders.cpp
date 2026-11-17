// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Project folders (E12): folder.list / create / rename / move / delete, project.move, and the folder filter of project.list / project.catalog.
// Folders are a per-tenant document beside the projects (ProjectFolders): none of these commands touches a project, its revision or its undo history.
#include "Command.h"

#include "Ids.h"
#include "ProjectManager.h"

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

json strMin (int n)         { return { { "type", "string" }, { "minLength", n } }; }
json folderRef()            { return { { "type", "string" }, { "minLength", 1 }, { "description", "folder_id from folder.list; \"root\" = top level, \"tests\" = the system folder" } }; }

json topOrId (const std::string& folderId)
{
    return folderId.empty() || folderId == ProjectFolders::rootId ? json (nullptr) : json (folderId);
}

json folderResult (ProjectManager& pm, const std::string& tenant, const std::string& folderId)
{
    const auto doc = pm.folders().read (tenant);
    const auto* f = doc.find (folderId);

    if (f == nullptr)
        return nullptr;

    return ProjectFolders::folderJson (*f, doc, pm.knownProjects (tenant).ids);
}
} // namespace

/** Adds `folder_id` (null = top level) to every item and keeps those the folder params select. `catalog`: without folder_id the projects of the
    system folder `tests` are hidden unless include_system is true (project.list keeps them). */
Result<json> filterProjectsByFolder (ProjectManager& pm, const CommandContext& ctx, json items, const json& params, bool catalog)
{
    const auto doc = pm.folders().read (ctx.tenant);
    const bool includeSystem = params.value ("include_system", false);
    const bool recursive = params.value ("recursive", false);
    const std::string folder = params.value ("folder_id", "");
    std::set<std::string> wanted;

    if (! folder.empty() && folder != ProjectFolders::rootId)
    {
        if (doc.find (folder) == nullptr)
            return makeError (errc::notFound, "no folder " + folder, json { { "folder_id", folder } });

        wanted = recursive ? doc.subtree (folder) : std::set<std::string> { folder };
    }

    json out = json::array();

    for (auto& item : items)
    {
        const auto at = doc.folderOf (item.value ("project_id", ""));
        item["folder_id"] = at.empty() ? json (nullptr) : json (at);
        bool keep = true;

        if (folder.empty() || (folder == ProjectFolders::rootId && recursive))
            keep = ! (catalog && at == ProjectFolders::testsId && ! includeSystem);
        else if (folder == ProjectFolders::rootId)
            keep = at.empty();
        else
            keep = wanted.count (at) != 0;

        if (keep)
            out.push_back (std::move (item));
    }

    return out;
}

/** The folders block of project.catalog include_folders. */
void addFoldersTo (json& result, ProjectManager& pm, const CommandContext& ctx)
{
    const auto doc = pm.folders().read (ctx.tenant);
    result["folders"] = ProjectFolders::foldersJson (doc, pm.knownProjects (ctx.tenant).ids);
    result["folders_version"] = doc.version;
}

/** Assigns a project made by project.new / template.new to a folder; a failure is a warning in the result, the project stays. */
void assignNewProject (ProjectManager& pm, const CommandContext& ctx, const std::string& projectId, const std::string& folderId, json& result)
{
    if (folderId.empty() || folderId == ProjectFolders::rootId)
        return;

    auto assigned = pm.folders().assign (ctx.tenant, { projectId }, folderId);

    if (! assigned.ok())
    {
        result["warning"] = "the project was created at the top level: it could not be put into folder " + folderId + " (" + assigned.error().message + ")";
        return;
    }

    result["folder_id"] = folderId;
    pm.publishFoldersChanged (ctx.tenant, assigned.value().version, "projects_moved", { { "folder_id", folderId }, { "project_ids", json::array ({ projectId }) } });
}

/** NOT_FOUND before anything is created when project.new / template.new name a folder that does not exist. */
Result<void> checkFolderForNewProject (ProjectManager& pm, const CommandContext& ctx, const std::string& folderId)
{
    if (folderId.empty() || folderId == ProjectFolders::rootId)
        return {};

    if (pm.folders().read (ctx.tenant).find (folderId) == nullptr)
        return makeError (errc::notFound, "no folder " + folderId, json { { "folder_id", folderId } });

    return {};
}

void registerFolderCommands (CommandRegistry& r)
{
    r.add (makeGlobalCommand ("folder.list", obj ({}), obj ({ { "folders", { { "type", "array" } } }, { "version", { { "type", "integer" } } } }), false,
        [] (ProjectManager& pm, const json&, const CommandContext& ctx) -> Result<json>
        {
            const auto doc = pm.folders().read (ctx.tenant);
            return json { { "folders", ProjectFolders::foldersJson (doc, pm.knownProjects (ctx.tenant).ids) }, { "version", doc.version } };
        }));

    r.add (makeGlobalCommand ("folder.create", obj ({ { "name", strMin (1) }, { "parent_id", folderRef() } }, { "name" }), obj ({ { "folder", { { "type", "object" } } } }), true,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            auto made = pm.folders().create (ctx.tenant, p["name"].get<std::string>(), p.value ("parent_id", ""));

            if (! made.ok())
                return made.error();

            const auto& f = made.value().folder;
            pm.publishFoldersChanged (ctx.tenant, made.value().version, "created", { { "folder_id", f.id } });
            return json { { "folder", folderResult (pm, ctx.tenant, f.id) } };
        }));

    r.add (makeGlobalCommand ("folder.rename", obj ({ { "folder_id", strMin (1) }, { "name", strMin (1) } }, { "folder_id", "name" }), obj ({ { "folder", { { "type", "object" } } } }), true,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            auto renamed = pm.folders().rename (ctx.tenant, p["folder_id"].get<std::string>(), p["name"].get<std::string>());

            if (! renamed.ok())
                return renamed.error();

            const auto& f = renamed.value().folder;
            pm.publishFoldersChanged (ctx.tenant, renamed.value().version, "renamed", { { "folder_id", f.id } });
            return json { { "folder", folderResult (pm, ctx.tenant, f.id) } };
        }));

    r.add (makeGlobalCommand ("folder.move", obj ({ { "folder_id", strMin (1) }, { "parent_id", folderRef() } }, { "folder_id", "parent_id" }), obj ({ { "folder", { { "type", "object" } } } }), true,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            auto moved = pm.folders().move (ctx.tenant, p["folder_id"].get<std::string>(), p["parent_id"].get<std::string>());

            if (! moved.ok())
                return moved.error();

            const auto& f = moved.value().folder;
            pm.publishFoldersChanged (ctx.tenant, moved.value().version, "moved", { { "folder_id", f.id } });
            return json { { "folder", folderResult (pm, ctx.tenant, f.id) } };
        }));

    r.add (makeGlobalCommand ("folder.delete", obj ({ { "folder_id", strMin (1) } }, { "folder_id" }), obj ({}), true,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            auto removed = pm.folders().remove (ctx.tenant, p["folder_id"].get<std::string>());

            if (! removed.ok())
                return removed.error();

            pm.publishFoldersChanged (ctx.tenant, removed.value().version, "deleted", { { "folder_id", removed.value().folder.id } });
            return json::object();
        }));

    r.add (makeGlobalCommand ("project.move",
        obj ({ { "project_id", strMin (1) },
               { "project_ids", { { "type", "array" }, { "items", strMin (1) }, { "minItems", 1 }, { "maxItems", 100 } } },
               { "folder_id", folderRef() } }, { "folder_id" }),
        obj ({ { "moved", { { "type", "integer" } } }, { "folder_id", { { "type", { "string", "null" } } } } }), true,
        [] (ProjectManager& pm, const json& p, const CommandContext& ctx) -> Result<json>
        {
            if (p.contains ("project_id") == p.contains ("project_ids"))
                return makeError (errc::invalidParams, "give either project_id or project_ids", json { { "path", "/project_ids" } });

            std::vector<std::string> ids;
            std::set<std::string> seen;

            for (auto& id : p.contains ("project_id") ? json::array ({ p["project_id"] }) : p["project_ids"])
                if (seen.insert (id.get<std::string>()).second)
                    ids.push_back (id.get<std::string>());

            // all or nothing: one unknown project (or another tenant's) moves none
            json missing = json::array();

            for (auto& id : ids)
                if (! pm.projectExists (ctx.tenant, id))
                    missing.push_back (id);

            if (! missing.empty())
                return makeError (errc::notFound, missing.size() == 1 ? "no project " + missing[0].get<std::string>() : "no projects " + missing.dump(),
                                  json { { "project_ids", missing } });

            const auto folder = p["folder_id"].get<std::string>();
            auto moved = pm.folders().assign (ctx.tenant, ids, folder);

            if (! moved.ok())
                return moved.error();

            pm.publishFoldersChanged (ctx.tenant, moved.value().version, "projects_moved", { { "folder_id", topOrId (folder) }, { "project_ids", ids } });
            return json { { "moved", ids.size() }, { "folder_id", topOrId (folder) } };
        }));
}

} // namespace rovox
