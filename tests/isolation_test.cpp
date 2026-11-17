// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E11-04: tenant isolation. Two tenants share one server; nothing of tenant A may be read, changed or reached by tenant B: not through any
// MCP tool (generated from the tool registry), not through paths, uploads, recipes or WebSocket events. Tenant ids are unique to this file
// (`iso_*`) because every test of the process shares one data directory.
#include "Auth.h"
#include "Gateway.h"
#include "JobManager.h"
#include "Mcp.h"
#include "PluginCatalog.h"
#include "Quota.h"
#include "RenderJobs.h"
#include "TestFramework.h"
#include "TestSupport.h"
#include "UploadStore.h"

#include <atomic>
#include <fstream>
#include <set>
#include <thread>

using namespace rovox;

namespace
{
/** Same shape as the MCP test harness: tool calls block on the message thread, so they run on a client thread while this thread pumps. */
struct Harness
{
    explicit Harness (mcp::McpConfig cfg = {})
        : jobs (fs::path {}), service (env.pm, jobs, env.executor), server (env.pm, env.bus, env.hub, jobs, service, std::move (cfg))
    {
    }

    ~Harness() { jobs.shutdown(); }

    std::shared_ptr<mcp::Session> session (const std::string& tenant)
    {
        auto s = server.createSession (tenant);
        rpc (s, "initialize", { { "protocolVersion", "2025-06-18" }, { "capabilities", json::object() }, { "clientInfo", { { "name", "iso" }, { "version", "1" } } } });
        mcp::RequestContext rc;
        server.handle (*s, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }, rc);
        return s;
    }

    json rpc (const std::shared_ptr<mcp::Session>& s, const std::string& method, json params)
    {
        json req { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", method }, { "params", std::move (params) } };
        std::atomic<bool> done { false };
        json out;

        std::thread t ([&]
        {
            mcp::RequestContext rc;
            auto r = server.handle (*s, req, rc);
            out = r ? *r : json (nullptr);
            done = true;
        });

        while (! done)
            test::pumpMessages (2);

        t.join();
        return out;
    }

    /** tools/call; the structuredContent {ok, data | error}. */
    json tool (const std::shared_ptr<mcp::Session>& s, const std::string& name, json args = json::object())
    {
        auto r = rpc (s, "tools/call", { { "name", name }, { "arguments", std::move (args) } });

        if (! r.contains ("result"))
            return { { "ok", false }, { "protocol_error", r.value ("error", json (nullptr)) } };

        return r["result"]["structuredContent"];
    }

    test::Env env;
    JobManager jobs;
    JobService service;
    mcp::McpServer server;
};

void removeTenantDirs (const std::vector<std::string>& tenants)
{
    std::error_code ec;

    for (auto& t : tenants)
        fs::remove_all (test::host().dataDir() / "tenants" / t, ec);
}

/** Property names that carry the id of a resource of tenant A, and the value tenant B is handed for them. */
struct Resources
{
    std::string project, track, item, job, upload, batch, recipe;
};

/** Arguments for `tool` built from its input schema: A's ids for resource properties, a type-appropriate dummy for the rest that is required. */
json argsFor (const mcp::ToolDef& tool, const Resources& a, bool& touchesResource)
{
    json args = json::object();
    touchesResource = false;
    const std::map<std::string, std::string> ids {
        { "project_id", a.project }, { "track_id", a.track }, { "item_id", a.item }, { "job_id", a.job }, { "upload_id", a.upload },
        { "batch_id", a.batch }, { "recipe_id", a.recipe } };

    if (! tool.inputSchema.contains ("properties"))
        return args;

    std::set<std::string> required;

    if (tool.inputSchema.contains ("required"))
        for (auto& r : tool.inputSchema["required"])
            required.insert (r.get<std::string>());

    for (auto& [name, schema] : tool.inputSchema["properties"].items())
    {
        if (auto id = ids.find (name); id != ids.end() && ! id->second.empty())
        {
            args[name] = id->second;
            touchesResource = true;
        }
        else if (name == "wait_s")
        {
            args[name] = 0;                                    // never block on a job of the other tenant
        }
        else if (required.count (name) != 0)
        {
            const std::string type = schema.is_object() && schema.contains ("type") && schema["type"].is_string() ? schema["type"].get<std::string>() : "string";

            if (schema.is_object() && schema.contains ("enum") && schema["enum"].is_array() && ! schema["enum"].empty())     args[name] = schema["enum"][0];
            else if (type == "number" || type == "integer")               args[name] = schema.is_object() && schema.contains ("minimum") && schema["minimum"].is_number() ? schema["minimum"] : json (1);
            else if (type == "boolean")                                   args[name] = false;
            else if (type == "array")                                     args[name] = json::array();
            else if (type == "object")                                    args[name] = json::object();
            else                                                          args[name] = "iso-x";
        }
    }

    return args;
}
} // namespace

ROVOX_TEST (isolation, no_tool_gives_tenant_b_the_data_or_the_control_of_tenant_a)
{
    removeTenantDirs ({ "iso_a", "iso_b" });
    Harness h;
    auto a = h.session ("iso_a");
    auto b = h.session ("iso_b");

    const std::string secret = "iso-secret-project-name", secretTrack = "iso-secret-track-name";
    Resources res;
    auto project = h.tool (a, "project.new", { { "name", secret } });
    REQUIRE (project["ok"] == true);
    res.project = project["data"]["project_id"];
    auto track = h.tool (a, "track.create", { { "project_id", res.project }, { "name", secretTrack } });
    REQUIRE (track["ok"] == true);
    res.track = track["data"]["track_id"];

    // an audio clip, so that there is an item and a render to reach for
    const auto dir = test::host().dataDir() / "tenants" / "iso_a" / "media";       // a tenant imports only from its own subtree
    fs::create_directories (dir);
    test::writeWav (dir / "a.wav", { test::sine (440.0, -20.0, 2.0, 44100.0) }, 44100.0, 24);
    auto plan = h.tool (a, "import.plan", { { "project_id", res.project }, { "paths", json::array ({ (dir / "a.wav").string() }) }, { "wait_s", 30 } });

    REQUIRE (plan["ok"] == true);
    REQUIRE (plan["data"].contains ("result"));
    CHECK (h.tool (a, "import.apply", { { "project_id", res.project }, { "plan_id", plan["data"]["result"]["plan_id"] } })["ok"] == true);

    auto got = h.tool (a, "project.get", { { "project_id", res.project } });
    REQUIRE (got["ok"] == true);

    for (auto& t : got["data"].value ("tracks", json::array()))
        for (auto& it : t.value ("items", json::array()))
            if (res.item.empty())
                res.item = it.value ("item_id", it.value ("id", ""));

    auto render = h.tool (a, "render.start", { { "project_id", res.project }, { "name", "iso" } });
    if (render["ok"] == true)
        res.job = render["data"]["job_id"];
    else
        std::cerr << "    render.start: " << render.dump() << "\n";

    CHECK (! res.job.empty());

    auto recipe = h.tool (a, "recipe.save", { { "recipe", { { "id", "iso-recipe-a" }, { "name", "A only" }, { "steps", json::array ({ { { "tool", "project.get" } } }) } } } });
    CHECK (recipe["ok"] == true);
    res.recipe = "iso-recipe-a";

    const auto before = h.tool (a, "project.get", { { "project_id", res.project } });
    REQUIRE (before["ok"] == true);

    // tools that legitimately answer ok for an id they do not own: they only touch the caller's own session state or return what the caller may see
    const std::set<std::string> okAllowed { "events.subscribe", "events.poll" };

    int exercised = 0;
    std::vector<std::string> leaks;

    for (auto* tool : h.server.tools())
    {
        if (tool->name.rfind ("admin", 0) == 0)
            continue;                                            // operator-only (tenant `local` holds the admin scope)

        bool touches = false;
        auto args = argsFor (*tool, res, touches);

        if (! touches)
            continue;                                            // global catalogs and constructors have nothing of A's to reach for

        ++exercised;
        auto r = h.tool (b, tool->name, args);
        const auto text = r.dump();

        if (text.find (secret) != std::string::npos || text.find (secretTrack) != std::string::npos)
            leaks.push_back (tool->name + " returned data of tenant A");

        if (r.value ("ok", false) && okAllowed.count (tool->name) == 0)
            leaks.push_back (tool->name + " answered ok for tenant A's resources");
    }

    for (auto& l : leaks)
        std::cerr << "    LEAK: " << l << "\n";

    CHECK (leaks.empty());
    CHECK (exercised >= 20);                                     // the generator found the project, track, item, job and recipe tools

    // nothing of A changed (revision and content), and B sees none of it in its own listings
    const auto after = h.tool (a, "project.get", { { "project_id", res.project } });
    REQUIRE (after["ok"] == true);
    CHECK (after["data"] == before["data"]);
    CHECK (after.value ("revision", 0) == before.value ("revision", 0));

    const auto listed = h.tool (b, "project.list").dump();
    CHECK (listed.find (res.project) == std::string::npos);
    CHECK (listed.find (secret) == std::string::npos);
    CHECK (h.tool (a, "project.list").dump().find (res.project) != std::string::npos);

    if (! res.job.empty())
    {
        CHECK (h.tool (b, "job.list").dump().find (res.job) == std::string::npos);
        CHECK (h.tool (a, "job.get", { { "job_id", res.job }, { "wait_s", 30 } })["ok"] == true);
    }

    removeTenantDirs ({ "iso_a", "iso_b" });
}

ROVOX_TEST (isolation, recipes_are_per_tenant_and_the_built_in_ones_are_shared)
{
    removeTenantDirs ({ "iso_ra", "iso_rb" });
    Harness h;
    auto a = h.session ("iso_ra");
    auto b = h.session ("iso_rb");

    json recipe { { "id", "iso-private" }, { "name", "private" }, { "steps", json::array ({ { { "tool", "project.list" } } }) } };
    REQUIRE (h.tool (a, "recipe.save", { { "recipe", recipe } })["ok"] == true);

    auto ids = [&] (const std::shared_ptr<mcp::Session>& s)
    {
        std::set<std::string> out;
        auto r = h.tool (s, "recipe.list");
        REQUIRE (r["ok"] == true);

        for (auto& x : r["data"]["recipes"])
            out.insert (x["recipe_id"].get<std::string>());

        return out;
    };

    const auto seenByA = ids (a), seenByB = ids (b);
    CHECK (seenByA.count ("iso-private") == 1);
    CHECK (seenByB.count ("iso-private") == 0);
    CHECK (seenByB.size() > 0 && seenByA.size() == seenByB.size() + 1);     // the built-in recipes are visible to both

    CHECK (h.tool (a, "recipe.get", { { "recipe_id", "iso-private" } })["ok"] == true);
    CHECK (h.tool (b, "recipe.get", { { "recipe_id", "iso-private" } })["ok"] == false);
    CHECK (h.tool (b, "recipe.run", { { "recipe_id", "iso-private" } })["ok"] == false);
    CHECK (h.tool (b, "recipe.delete", { { "recipe_id", "iso-private" } })["error"]["code"] == "NOT_FOUND");

    // the same id may exist for both tenants without collision
    json other = recipe;
    other["name"] = "b's own";
    REQUIRE (h.tool (b, "recipe.save", { { "recipe", other } })["ok"] == true);
    CHECK (h.tool (a, "recipe.get", { { "recipe_id", "iso-private" } })["data"]["recipe"]["name"] == "private");
    CHECK (h.tool (b, "recipe.get", { { "recipe_id", "iso-private" } })["data"]["recipe"]["name"] == "b's own");

    CHECK (fs::exists (test::host().dataDir() / "tenants" / "iso_ra" / "recipes" / "iso-private.json"));
    CHECK (h.tool (a, "recipe.delete", { { "recipe_id", "iso-private" } })["ok"] == true);
    CHECK (h.tool (b, "recipe.get", { { "recipe_id", "iso-private" } })["ok"] == true);        // b's copy is untouched

    removeTenantDirs ({ "iso_ra", "iso_rb" });
}

ROVOX_TEST (isolation, check_path_confines_a_tenant_to_its_own_subtree_dotdot_and_symlinks_included)
{
    removeTenantDirs ({ "iso_pa", "iso_pb" });
    auto& host = test::host();
    const auto tenants = host.dataDir() / "tenants";
    fs::create_directories (tenants / "iso_pa");
    fs::create_directories (tenants / "iso_pb");
    std::ofstream (tenants / "iso_pb" / "secret.wav") << "b";
    std::ofstream (tenants / "iso_pa" / "mine.wav") << "a";

    const auto shared = test::makeTempDir ("iso-shared");
    std::ofstream (shared / "tool.wav") << "s";
    std::error_code ec;
    fs::create_directory_symlink (tenants / "iso_pb", tenants / "iso_pa" / "link", ec);
    REQUIRE (! ec);

    struct Policy
    {
        explicit Policy (std::vector<fs::path> roots) { test::host().setPathPolicy (true, std::move (roots)); }
        ~Policy() { test::host().setPathPolicy (false, {}); }
    } policy ({ shared });

    // own subtree, shared tool root
    CHECK (host.checkPath (tenants / "iso_pa" / "mine.wav", "iso_pa").ok());
    CHECK (host.checkPath (tenants / "iso_pa" / "not-yet" / "new.wav", "iso_pa").ok());
    CHECK (host.checkPath (shared / "tool.wav", "iso_pa").ok());

    // the neighbour, the operator's area, the data dir itself
    auto other = host.checkPath (tenants / "iso_pb" / "secret.wav", "iso_pa");
    REQUIRE (! other.ok());
    CHECK (other.error().code == errc::forbiddenPath);
    CHECK (! host.checkPath (host.dataDir() / "projects", "iso_pa").ok());
    CHECK (! host.checkPath (host.dataDir() / "uploads", "iso_pa").ok());
    CHECK (! host.checkPath (host.dataDir(), "iso_pa").ok());
    CHECK (! host.checkPath (tenants, "iso_pa").ok());

    // `..` out of its own directory, and a symlink inside it that points at the neighbour
    CHECK (! host.checkPath (tenants / "iso_pa" / ".." / "iso_pb" / "secret.wav", "iso_pa").ok());
    CHECK (! host.checkPath (tenants / "iso_pa" / "link" / "secret.wav", "iso_pa").ok());
    CHECK (! host.checkPath (tenants / "iso_pa" / "mine.wav" / ".." / ".." / "iso_pb" / "secret.wav", "iso_pa").ok());

    // a tenant id that is not a tenant id gets no subtree at all
    CHECK (! host.checkPath (tenants / "iso_pa" / "mine.wav", "../iso_pa").ok());
    CHECK (! host.checkPath (host.dataDir() / "x.wav", "").ok());

    // the operator keeps the whole data dir
    CHECK (host.checkPath (tenants / "iso_pb" / "secret.wav").ok());
    CHECK (host.checkPath (host.dataDir() / "projects", "local").ok());

    // end to end: tenant B cannot open tenant A's project directory by path
    {
        Harness h;
        auto a = h.session ("iso_pa");
        auto b = h.session ("iso_pb");
        auto project = h.tool (a, "project.new", { { "name", "iso-path" } });
        REQUIRE (project["ok"] == true);
        const std::string id = project["data"]["project_id"];
        CHECK (h.tool (a, "project.save", { { "project_id", id } })["ok"] == true);
        const auto dir = h.env.pm.projectDir ("iso_pa", id);
        auto opened = h.tool (b, "project.open", { { "target", dir.string() } });
        CHECK (opened["ok"] == false);
        CHECK (opened["error"]["code"] == "FORBIDDEN_PATH");
        h.tool (a, "project.close", { { "project_id", id }, { "save", false } });
    }

    removeTenantDirs ({ "iso_pa", "iso_pb" });
}

ROVOX_TEST (isolation, upload_tools_do_not_show_or_materialise_another_tenants_batch)
{
    const auto dir = test::makeTempDir ("iso-uploads");
    UploadStore store (dir);
    mcp::McpConfig cfg;
    cfg.uploadBatchStatus = [&store] (const std::string& id, const std::string& tenant) { return store.batchStatus (id, tenant); };
    cfg.uploadFinish = [&store] (const std::string& id, const std::string& tenant) { return store.finish (id, tenant); };
    Harness h (cfg);
    auto a = h.session ("iso_ua");
    auto b = h.session ("iso_ub");

    auto batch = store.createBatch (json::array ({ { { "relpath", "x.wav" }, { "size", 3 } } }), "iso_ua");
    REQUIRE (batch.ok());
    const std::string batchId = batch.value()["batch_id"], uploadId = batch.value()["files"][0]["upload_id"];
    REQUIRE (store.append (uploadId, 0, "abc", 3, "iso_ua").ok());

    CHECK (h.tool (a, "upload.status", { { "batch_id", batchId } })["ok"] == true);
    auto denied = h.tool (b, "upload.status", { { "batch_id", batchId } });
    CHECK (denied["ok"] == false);
    CHECK (denied["error"]["code"] == "NOT_FOUND");

    CHECK (h.tool (b, "upload.finish", { { "batch_id", batchId } })["error"]["code"] == "NOT_FOUND");
    CHECK (h.tool (b, "upload.finish", { { "upload_id", uploadId } })["ok"] == false);
    CHECK (! fs::exists (store.materialisedDir (batchId)));             // B's attempts did not lay A's files out

    auto finished = h.tool (a, "upload.finish", { { "batch_id", batchId } });
    CHECK (finished["ok"] == true);
    CHECK (fs::exists (store.materialisedDir (batchId)));
}

ROVOX_TEST (isolation, websocket_events_never_cross_tenants)
{
    auto event = [] (const std::string& tenant, const std::string& project, const std::string& type)
    {
        Event e;
        e.tenant = tenant;
        e.projectId = project;
        e.body = json { { "type", type } };
        return e;
    };

    std::map<std::string, Gateway::Sub> subs;
    subs["p_a"] = Gateway::Sub {};              // the client subscribed to p_a (it could only do so for a project of its own tenant)

    // right tenant: delivered as before
    CHECK (Gateway::shouldDeliver ("iso_a", subs, event ("iso_a", "p_a", "project.changed")));
    CHECK (Gateway::shouldDeliver ("iso_a", subs, event ("iso_a", "p_a", "transport.changed")));
    CHECK (Gateway::shouldDeliver ("iso_a", subs, event ("iso_a", "", "upload.completed")));
    CHECK (Gateway::shouldDeliver ("iso_a", subs, event ("iso_a", "p_other", "job.updated")));
    CHECK (! Gateway::shouldDeliver ("iso_a", subs, event ("iso_a", "p_other", "project.changed")));       // not subscribed

    // another tenant's client gets none of them, not even for a project id it subscribed to
    CHECK (! Gateway::shouldDeliver ("iso_b", subs, event ("iso_a", "p_a", "project.changed")));
    CHECK (! Gateway::shouldDeliver ("iso_b", subs, event ("iso_a", "p_a", "transport.changed")));
    CHECK (! Gateway::shouldDeliver ("iso_b", subs, event ("iso_a", "p_a", "job.updated")));
    CHECK (! Gateway::shouldDeliver ("iso_b", subs, event ("iso_a", "", "upload.completed")));
    CHECK (! Gateway::shouldDeliver ("iso_b", subs, event ("iso_a", "", "usage.recorded")));
    CHECK (! Gateway::shouldDeliver ("iso_b", {}, event ("iso_a", "", "job.updated")));

    // channels still apply
    Gateway::Sub stateOnly;
    stateOnly.transport = false;
    std::map<std::string, Gateway::Sub> narrow { { "p_a", stateOnly } };
    CHECK (Gateway::shouldDeliver ("iso_a", narrow, event ("iso_a", "p_a", "project.changed")));
    CHECK (! Gateway::shouldDeliver ("iso_a", narrow, event ("iso_a", "p_a", "transport.changed")));
}

ROVOX_TEST (isolation, licence_policy_comes_from_the_tenant_record_and_fails_closed)
{
    const auto dir = test::makeTempDir ("iso-licence");
    const auto config = dir / "rovox.toml";
    {
        std::ofstream out (config);
        out << "[tenants.iso_open]\nlicence_policy = \"any\"\n\n[tenants.iso_strict]\nlicence_policy = \"server_use_ok\"\n\n[tenants.iso_profile]\nquota_profile = \"default\"\n";
    }

    quotas().setConfigFile (config);

    const std::optional<json> freeRec = json { { "server_use_ok", true } }, unknown = json { { "server_use_ok", "unknown" } }, missing = std::nullopt;

    CHECK (! PluginCatalog::licenceGated ("local"));                    // the operator is unchanged
    CHECK (! PluginCatalog::licenceGated ("iso_open"));                 // an explicit "any" lifts the gate
    CHECK (PluginCatalog::licenceGated ("iso_strict"));
    CHECK (PluginCatalog::licenceGated ("iso_profile"));                // a record without a policy: restricted
    CHECK (PluginCatalog::licenceGated ("iso_no_record"));              // no record at all: restricted (fail closed)

    CHECK (PluginCatalog::licenceAllows ("local", missing));
    CHECK (PluginCatalog::licenceAllows ("iso_open", unknown));
    CHECK (PluginCatalog::licenceAllows ("iso_open", missing));
    CHECK (PluginCatalog::licenceAllows ("iso_strict", freeRec));
    CHECK (! PluginCatalog::licenceAllows ("iso_strict", unknown));
    CHECK (! PluginCatalog::licenceAllows ("iso_strict", missing));
    CHECK (! PluginCatalog::licenceAllows ("iso_no_record", unknown));
    CHECK (PluginCatalog::licenceAllows ("iso_no_record", freeRec));

    CHECK (! PluginCatalog::requireLicence ("iso_strict", unknown, "x").ok());
    CHECK (PluginCatalog::requireLicence ("iso_open", unknown, "x").ok());

    quotas().setConfigFile ({});
}

ROVOX_TEST (isolation, folders_are_per_tenant)
{
    removeTenantDirs ({ "iso_fa", "iso_fb" });
    Harness h;
    auto a = h.session ("iso_fa");
    auto b = h.session ("iso_fb");
    const std::string secret = "iso-secret-folder-name";

    auto project = h.tool (a, "project.new", { { "name", "iso folder project" } });
    REQUIRE (project["ok"] == true);
    const auto projectId = project["data"]["project_id"].get<std::string>();
    auto folder = h.tool (a, "folder.create", { { "name", secret } });
    REQUIRE (folder["ok"] == true);
    const auto folderId = folder["data"]["folder"]["folder_id"].get<std::string>();
    REQUIRE (h.tool (a, "project.move", { { "project_id", projectId }, { "folder_id", folderId } })["ok"] == true);

    // B sees only its own folders (and the system one), and cannot reach A's folder or project by id
    auto listed = h.tool (b, "folder.list");
    REQUIRE (listed["ok"] == true);
    CHECK (listed.dump().find (secret) == std::string::npos);
    CHECK (listed["data"]["folders"].size() == 1);
    CHECK (listed["data"]["folders"][0]["folder_id"] == "tests");
    CHECK (h.tool (b, "project.catalog", { { "include_folders", true } }).dump().find (secret) == std::string::npos);

    CHECK (h.tool (b, "folder.rename", { { "folder_id", folderId }, { "name", "taken" } })["error"]["code"] == errc::notFound);
    CHECK (h.tool (b, "folder.move", { { "folder_id", folderId }, { "parent_id", "root" } })["error"]["code"] == errc::notFound);
    CHECK (h.tool (b, "folder.delete", { { "folder_id", folderId } })["error"]["code"] == errc::notFound);
    CHECK (h.tool (b, "project.catalog", { { "folder_id", folderId } })["error"]["code"] == errc::notFound);
    CHECK (h.tool (b, "project.move", { { "project_id", projectId }, { "folder_id", "tests" } })["error"]["code"] == errc::notFound);

    auto bFolder = h.tool (b, "folder.create", { { "name", secret } });         // the same name is free in B
    REQUIRE (bFolder["ok"] == true);
    CHECK (h.tool (b, "project.move", { { "project_id", projectId }, { "folder_id", bFolder["data"]["folder"]["folder_id"] } })["error"]["code"] == errc::notFound);
    CHECK (h.tool (a, "project.move", { { "project_id", projectId }, { "folder_id", bFolder["data"]["folder"]["folder_id"] } })["error"]["code"] == errc::notFound);

    // A's folder and assignment are as they were
    auto mine = h.tool (a, "project.catalog", { { "folder_id", folderId } });
    REQUIRE (mine["ok"] == true);
    REQUIRE (mine["data"]["projects"].size() == 1);
    CHECK (mine["data"]["projects"][0]["project_id"] == projectId);
    auto aFolders = h.tool (a, "folder.list")["data"]["folders"];
    CHECK (aFolders.size() == 2);
    CHECK (aFolders[0]["name"] == secret);
    CHECK (aFolders[0]["project_count"] == 1);

    h.tool (a, "project.delete", { { "project_id", projectId } });
    removeTenantDirs ({ "iso_fa", "iso_fb" });
}
