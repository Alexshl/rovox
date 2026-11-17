// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E12: project folders. The folders document (ProjectFolders) against a local file and an in-memory bucket (conditional writes, the read-back
// fallback, the cache, an unreachable bucket), the ProjectManager hooks (deleted projects drop their assignment) and the commands (folder.*,
// project.move, the folder filters of project.list / project.catalog, folders.changed). Tenant ids are unique to this file (`fold_*`) because
// every test of the process shares one data directory.
#include "FakeStore.h"
#include "Ids.h"
#include "ProjectFolders.h"
#include "RemoteStorage.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <fstream>
#include <set>
#include <thread>

using namespace rovox;

namespace
{
using test::FakeStore;

void removeTenantDirs (const std::vector<std::string>& tenants)
{
    std::error_code ec;

    for (auto& t : tenants)
        fs::remove_all (test::host().dataDir() / "tenants" / t, ec);
}

CommandResult runIn (test::Env& env, const std::string& tenant, const std::string& name, const json& params = json::object(), const std::string& project = "")
{
    CommandContext ctx;
    Principal p = Principal::local();
    p.tenant = tenant;
    ctx.setPrincipal (p);
    ctx.projectId = project;
    ctx.origin = { Origin::Kind::mcp, "mcp_1", "Claude" };
    return env.bus.runNow (name, params, ctx);
}

json okIn (test::Env& env, const std::string& tenant, const std::string& name, const json& params = json::object(), const std::string& project = "")
{
    auto r = runIn (env, tenant, name, params, project);

    if (! r.ok)
        std::cerr << "    command " << name << " failed: " << (r.error ? r.error->toJson().dump() : std::string ("?")) << "\n";

    REQUIRE (r.ok);
    return r.data;
}

std::string errorOf (const CommandResult& r)
{
    return r.ok || ! r.error ? std::string ("OK") : r.error->code;
}

std::string newIn (test::Env& env, const std::string& tenant, const std::string& name, bool ephemeral = false, std::optional<int> ttl = std::nullopt)
{
    CreateOptions o;
    o.name = name;
    o.tenant = tenant;
    o.ephemeral = ephemeral;
    o.ttlS = ttl;
    auto id = env.pm.create (o);
    REQUIRE (id.ok());
    return id.value();
}

std::set<std::string> idsOf (const json& projects)
{
    std::set<std::string> out;

    for (auto& p : projects)
        out.insert (p["project_id"].get<std::string>());

    return out;
}

const json* folderNamed (const json& folders, const std::string& name)
{
    for (auto& f : folders)
        if (f["name"] == name)
            return &f;

    return nullptr;
}

/** A ProjectFolders on its own: a local file below a temp dir and (optionally) a bucket. */
struct Folders
{
    explicit Folders (std::shared_ptr<FakeStore> s = nullptr, bool conditional = true, fs::path dir = test::makeTempDir ("folders"))
        : root (std::move (dir)), store (std::move (s)), pf ([r = root] (const std::string& t) { return r / t / "folders.json"; })
    {
        if (store)
            pf.setStore (store, StorageKeys { "rovox/" }, [conditional] { return conditional; });
    }

    fs::path root;
    std::shared_ptr<FakeStore> store;
    ProjectFolders pf;
};

std::string key (const std::string& tenant)
{
    return StorageKeys { "rovox/" }.folders (tenant);
}

bool waitFor (const std::function<bool()>& cond, int ms = 8000)
{
    for (int i = 0; i < ms / 20; ++i)
    {
        if (cond())
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    return cond();
}

/** A bucket store whose get() lets "another server" write the folders document right after it was read (`races` times). */
class RacingStore : public FakeStore
{
public:
    Result<std::string> get (const std::string& k) override
    {
        auto r = FakeStore::get (k);

        if (r.ok() && races > 0 && k.find ("folders.json") != std::string::npos)
        {
            --races;
            auto doc = json::parse (r.value());
            doc["version"] = doc.value ("version", 0) + 1;
            doc["folders"].push_back ({ { "folder_id", newId ("fld") }, { "name", "Other " + std::to_string (races) }, { "parent_id", nullptr },
                                        { "created_at", "" }, { "updated_at", "" } });
            setObject (k, doc.dump());
        }

        return r;
    }

    std::atomic<int> races { 0 };
};
} // namespace

//==============================================================================
// E12-01: the document

ROVOX_TEST (folders, tests_folder_always_present_and_immutable)
{
    Folders f;
    const std::string t = "fold_sys";

    auto doc = f.pf.read (t);
    REQUIRE (doc.find ("tests") != nullptr);
    CHECK (doc.find ("tests")->system);
    CHECK (doc.find ("tests")->name == "Тестові треки");

    auto list = ProjectFolders::foldersJson (doc, {});
    REQUIRE (list.size() == 1);
    CHECK (list[0]["folder_id"] == "tests");
    CHECK (list[0]["system"] == true);
    CHECK (list[0]["parent_id"].is_null());

    CHECK (f.pf.rename (t, "tests", "x").error().code == errc::preconditionFailed);
    CHECK (f.pf.move (t, "tests", "root").error().code == errc::preconditionFailed);
    CHECK (f.pf.remove (t, "tests").error().code == errc::preconditionFailed);
    CHECK (f.pf.create (t, "Sub", "tests").error().code == errc::invalidParams);       // no subfolders in tests

    auto a = f.pf.create (t, "A", "");
    REQUIRE (a.ok());
    CHECK (f.pf.move (t, a.value().folder.id, "tests").error().code == errc::invalidParams);

    // projects may go into tests; it is listed last even with folders sorted after it by name
    const auto p = newId ("p");
    REQUIRE (f.pf.assign (t, { p }, "tests").ok());
    CHECK (f.pf.read (t).folderOf (p) == "tests");
    REQUIRE (f.pf.create (t, "zzz", "").ok());
    list = ProjectFolders::foldersJson (f.pf.read (t), { p });
    REQUIRE (list.size() == 3);
    CHECK (list[0]["name"] == "A");
    CHECK (list[1]["name"] == "zzz");
    CHECK (list[2]["folder_id"] == "tests");
    CHECK (list[2]["project_count"] == 1);

    // a stored folder with the id `tests` is ignored: the system one is never stored
    fs::create_directories (f.root / "fold_sys2");
    std::ofstream (f.root / "fold_sys2" / "folders.json")
        << R"({"schema":1,"version":3,"folders":[{"folder_id":"tests","name":"Hijacked","parent_id":null}],"projects":{}})";
    auto stored = f.pf.read ("fold_sys2");
    CHECK (stored.folders.empty());
    CHECK (stored.find ("tests")->name == "Тестові треки");
    CHECK (stored.version == 3);
}

ROVOX_TEST (folders, create_rename_move_delete_round_trip_through_store)
{
    auto store = std::make_shared<FakeStore>();
    Folders f (store);
    const std::string t = "fold_rt";

    auto a = f.pf.create (t, "  Mixes  ", "root");
    REQUIRE (a.ok());
    CHECK (a.value().folder.name == "Mixes");                  // trimmed
    CHECK (isValidId (a.value().folder.id, "fld"));
    CHECK (a.value().version == 1);
    const auto aId = a.value().folder.id;

    auto b = f.pf.create (t, "Drafts", aId);
    REQUIRE (b.ok());
    CHECK (b.value().folder.parentId == aId);
    CHECK (b.value().version == 2);
    const auto bId = b.value().folder.id;

    // what the bucket holds: schema 1, no `tests`
    REQUIRE (store->objects.count (key (t)) == 1);
    auto stored = json::parse (store->objects[key (t)]);
    CHECK (stored["schema"] == 1);
    CHECK (stored["version"] == 2);
    CHECK (stored["folders"].size() == 2);

    REQUIRE (f.pf.rename (t, bId, "Old drafts").ok());
    auto moved = f.pf.move (t, bId, "root");
    REQUIRE (moved.ok());
    CHECK (moved.value().folder.parentId.empty());
    CHECK (moved.value().version == 4);

    // another server on the same bucket sees the same document
    Folders other (store);
    auto seen = other.pf.read (t);
    REQUIRE (seen.find (bId) != nullptr);
    CHECK (seen.find (bId)->name == "Old drafts");
    CHECK (seen.find (bId)->parentId.empty());
    CHECK (seen.version == 4);

    // its cache: a change made here shows there once the cache expires (revalidated by ETag)
    REQUIRE (f.pf.rename (t, aId, "Final mixes").ok());
    CHECK (other.pf.read (t).find (aId)->name == "Mixes");     // within 10 s: cached
    other.pf.setCacheTtl (std::chrono::milliseconds (0));
    CHECK (other.pf.read (t).find (aId)->name == "Final mixes");

    REQUIRE (f.pf.remove (t, bId).ok());
    auto last = f.pf.remove (t, aId);
    REQUIRE (last.ok());
    CHECK (last.value().version == 7);
    CHECK (other.pf.read (t).folders.empty());
    CHECK (f.pf.remove (t, aId).error().code == errc::notFound);
}

ROVOX_TEST (folders, move_refuses_cycles_and_depth_over_8)
{
    Folders f;
    const std::string t = "fold_depth";
    std::vector<std::string> chain;
    std::string parent = "root";

    for (int i = 1; i <= 8; ++i)
    {
        auto made = f.pf.create (t, "L" + std::to_string (i), parent);
        REQUIRE (made.ok());
        chain.push_back (made.value().folder.id);
        parent = chain.back();
    }

    CHECK (f.pf.read (t).depthOf (chain.back()) == 8);
    CHECK (f.pf.create (t, "L9", chain.back()).error().code == errc::invalidParams);

    // cycles
    CHECK (f.pf.move (t, chain[0], chain[0]).error().code == errc::invalidParams);
    CHECK (f.pf.move (t, chain[0], chain[3]).error().code == errc::invalidParams);
    CHECK (f.pf.move (t, chain[2], chain[7]).error().code == errc::invalidParams);
    CHECK (f.pf.move (t, chain[0], "fld_01JZZZZZZZZZZZZZZZZZZZZZZZ").error().code == errc::notFound);

    // a subtree of height 2 fits under level 6, not under level 7
    auto g = f.pf.create (t, "G", "root");
    REQUIRE (g.ok());
    REQUIRE (f.pf.create (t, "G2", g.value().folder.id).ok());
    CHECK (f.pf.move (t, g.value().folder.id, chain[6]).error().code == errc::invalidParams);
    CHECK (f.pf.move (t, g.value().folder.id, chain[5]).ok());

    // moving a branch up is fine
    CHECK (f.pf.move (t, chain[4], "root").ok());
    CHECK (f.pf.read (t).depthOf (chain.back()) == 4);
}

ROVOX_TEST (folders, delete_refuses_non_empty_counts_only_existing_projects)
{
    Folders f;
    const std::string t = "fold_del";
    const auto p1 = newId ("p"), p2 = newId ("p");
    KnownProjects known { { p1, p2 }, true };
    f.pf.setKnownProjects ([&] (const std::string&) { return known; });

    auto folder = f.pf.create (t, "Full", "");
    REQUIRE (folder.ok());
    const auto id = folder.value().folder.id;
    REQUIRE (f.pf.assign (t, { p1, p2 }, id).ok());

    auto refused = f.pf.remove (t, id);
    REQUIRE (! refused.ok());
    CHECK (refused.error().code == errc::preconditionFailed);
    CHECK (refused.error().details["projects"] == 2);
    CHECK (refused.error().details["folders"] == 0);

    // p2 was deleted elsewhere (its assignment is left over): only existing projects count
    known.ids = { p1 };
    refused = f.pf.remove (t, id);
    REQUIRE (! refused.ok());
    CHECK (refused.error().details["projects"] == 1);
    CHECK (ProjectFolders::folderJson (*f.pf.read (t).find (id), f.pf.read (t), known.ids)["project_count"] == 1);

    // a subfolder counts too
    auto sub = f.pf.create (t, "Sub", id);
    REQUIRE (sub.ok());
    known.ids.clear();
    refused = f.pf.remove (t, id);
    REQUIRE (! refused.ok());
    CHECK (refused.error().details["projects"] == 0);
    CHECK (refused.error().details["folders"] == 1);

    REQUIRE (f.pf.remove (t, sub.value().folder.id).ok());
    REQUIRE (f.pf.remove (t, id).ok());
    CHECK (f.pf.read (t).projects.empty());                  // the leftover assignments went with the write
}

ROVOX_TEST (folders, sibling_names_unique_case_insensitive)
{
    Folders f;
    const std::string t = "fold_names";

    auto mixes = f.pf.create (t, "Mixes", "");
    REQUIRE (mixes.ok());
    CHECK (f.pf.create (t, "mixes", "").error().code == errc::invalidParams);
    CHECK (f.pf.create (t, " MIXES ", "").error().code == errc::invalidParams);

    auto ua = f.pf.create (t, "Мікси Ґанку", "");
    REQUIRE (ua.ok());
    CHECK (f.pf.create (t, "МІКСИ ґАНКУ", "").error().code == errc::invalidParams);

    // the same name in another parent is fine; a rename onto a sibling's name is not, a change of case of its own name is
    CHECK (f.pf.create (t, "mixes", mixes.value().folder.id).ok());
    CHECK (f.pf.rename (t, ua.value().folder.id, "MIXES").error().code == errc::invalidParams);
    CHECK (f.pf.rename (t, mixes.value().folder.id, "MIXES").ok());

    // a move into a parent that already has that name
    auto other = f.pf.create (t, "Other", "");
    REQUIRE (other.ok());
    auto inner = f.pf.create (t, "Other", mixes.value().folder.id);
    REQUIRE (inner.ok());
    CHECK (f.pf.move (t, inner.value().folder.id, "root").error().code == errc::invalidParams);

    // names: trimmed, 1..100 characters, no '/' or control characters
    for (std::string bad : { std::string (""), std::string ("   "), std::string ("a/b"), std::string ("a\x01" "b"), std::string ("tab\there"),
                             std::string (101, 'x'), std::string ("\xff\xfe") })
        CHECK (f.pf.create (t, bad, "").error().code == errc::invalidParams);

    std::string hundred;

    for (int i = 0; i < 100; ++i)
        hundred += "ї";

    CHECK (f.pf.create (t, hundred, "").ok());
}

ROVOX_TEST (folders, conditional_write_retries_on_precondition_failure)
{
    auto store = std::make_shared<RacingStore>();
    Folders f (store, true);
    const std::string t = "fold_cond";

    REQUIRE (f.pf.create (t, "First", "").ok());

    // another server writes between our read and our conditional put: the put fails (412), we read again and re-apply
    store->races = 1;
    const auto putsBefore = store->countPuts ("folders.json");
    auto second = f.pf.create (t, "Second", "");
    REQUIRE (second.ok());
    CHECK (store->countPuts ("folders.json") == putsBefore + 1);       // the refused put is not logged; one put went through
    auto doc = FoldersDoc::fromJson (json::parse (store->objects[key (t)]));
    CHECK (doc.folders.size() == 3);                                   // First, the other server's, Second: nothing lost
    CHECK (doc.version == 3);
    CHECK (second.value().version == 3);

    // a writer that never stops racing: BUSY after 5 attempts, nothing of ours written
    store->races = 100;
    auto busy = f.pf.create (t, "Never", "");
    REQUIRE (! busy.ok());
    CHECK (busy.error().code == errc::busy);
    CHECK (store->races == 100 - ProjectFolders::maxAttempts);
    store->races = 0;
    CHECK (f.pf.read (t).folders.size() == 3 + ProjectFolders::maxAttempts);
    CHECK (folderNamed (ProjectFolders::foldersJson (f.pf.read (t), {}), "Never") == nullptr);
}

ROVOX_TEST (folders, fallback_without_conditional_writes_verifies_version)
{
    auto store = std::make_shared<FakeStore>();
    store->honourConditions = false;
    Folders f (store, false);
    const std::string t = "fold_fallback";

    REQUIRE (f.pf.create (t, "Mine", "").ok());

    // right after our plain put, another server overwrites the document with its own version 2: the read-back shows a different document,
    // so we read it again and re-apply our change on top of it
    int overwrites = 1;
    const auto otherId = newId ("fld");
    store->afterPut = [&] (const std::string& k)
    {
        if (k != key (t) || overwrites == 0)
            return;

        --overwrites;
        json doc { { "schema", 1 }, { "version", 2 }, { "updated_at", "" },
                   { "folders", json::array ({ { { "folder_id", otherId }, { "name", "Theirs" }, { "parent_id", nullptr } } }) }, { "projects", json::object() } };
        store->setObject (k, doc.dump());
    };

    auto mine = f.pf.create (t, "Mine too", "");
    REQUIRE (mine.ok());
    CHECK (overwrites == 0);
    auto doc = f.pf.read (t);
    CHECK (doc.find (otherId) != nullptr);
    CHECK (doc.find (mine.value().folder.id) != nullptr);
    CHECK (doc.version == 3);
    CHECK (json::parse (store->objects[key (t)])["version"] == 3);
}

ROVOX_TEST (folders, survive_restart)
{
    const std::string t = "fold_restart";
    removeTenantDirs ({ t });
    std::string folderId, projectId;

    {
        test::Env env;
        projectId = newIn (env, t, "Keeps its folder");
        folderId = okIn (env, t, "folder.create", { { "name", "Album" } })["folder"]["folder_id"].get<std::string>();
        okIn (env, t, "project.move", { { "project_id", projectId }, { "folder_id", folderId } });
        REQUIRE (env.pm.close (projectId, SaveMode::save).ok());
    }

    CHECK (fs::exists (test::host().dataDir() / "tenants" / t / "folders.json"));      // beside projects/, not inside

    {
        test::Env env;
        auto folders = okIn (env, t, "folder.list")["folders"];
        REQUIRE (folderNamed (folders, "Album") != nullptr);
        CHECK ((*folderNamed (folders, "Album"))["project_count"] == 1);
        auto in = okIn (env, t, "project.catalog", { { "folder_id", folderId } })["projects"];
        REQUIRE (in.size() == 1);
        CHECK (in[0]["project_id"] == projectId);
        CHECK (in[0]["folder_id"] == folderId);
    }

    // with a bucket: a new server (fresh cache, empty local copy) reads the same document
    auto store = std::make_shared<FakeStore>();
    std::string bucketFolder;
    {
        Folders first (store);
        bucketFolder = first.pf.create (t, "In the bucket", "").value().folder.id;
    }
    Folders second (store);
    CHECK (second.pf.read (t).find (bucketFolder) != nullptr);
    removeTenantDirs ({ t });
}

ROVOX_TEST (folders, tenants_are_isolated)
{
    auto store = std::make_shared<FakeStore>();
    Folders f (store);
    auto a = f.pf.create ("fold_ta", "Only A", "");
    REQUIRE (a.ok());
    CHECK (f.pf.read ("fold_tb").folders.empty());
    CHECK (f.pf.rename ("fold_tb", a.value().folder.id, "x").error().code == errc::notFound);
    CHECK (f.pf.assign ("fold_tb", { newId ("p") }, a.value().folder.id).error().code == errc::notFound);
    CHECK (f.pf.create ("fold_tb", "Only A", "").ok());                // names are per tenant too
    CHECK (store->objects.count (key ("fold_ta")) == 1);
    CHECK (store->objects.count (key ("fold_tb")) == 1);
    CHECK (fs::exists (f.root / "fold_ta" / "folders.json"));           // local copies per tenant
    CHECK (fs::exists (f.root / "fold_tb" / "folders.json"));
}

ROVOX_TEST (folders, deleting_project_drops_assignment)
{
    const std::string t = "fold_drop";
    removeTenantDirs ({ t });
    test::Env env;
    const auto folderId = okIn (env, t, "folder.create", { { "name", "Doomed" } })["folder"]["folder_id"].get<std::string>();
    auto assigned = [&] (const std::string& id) { return env.pm.folders().read (t).projects.count (id) != 0; };

    // an open project deleted with project.delete
    const auto open = newIn (env, t, "open one");
    okIn (env, t, "project.move", { { "project_id", open }, { "folder_id", folderId } });
    REQUIRE (assigned (open));
    okIn (env, t, "project.delete", { { "project_id", open } });
    CHECK (! assigned (open));

    // a closed project that only exists on disk
    const auto closed = newIn (env, t, "closed one");
    REQUIRE (env.pm.close (closed, SaveMode::save).ok());
    okIn (env, t, "project.move", { { "project_id", closed }, { "folder_id", folderId } });
    REQUIRE (assigned (closed));
    REQUIRE (env.pm.remove (closed, t).ok());
    CHECK (! assigned (closed));

    // ephemeral projects (they may be in folders) swept by the TTL: open and on disk
    const auto sweptOpen = newIn (env, t, "ttl open", true, 1);
    const auto sweptDisk = newIn (env, t, "ttl disk", true, 1);
    REQUIRE (env.pm.close (sweptDisk, SaveMode::save).ok());
    okIn (env, t, "project.move", { { "project_ids", { sweptOpen, sweptDisk } }, { "folder_id", folderId } });
    REQUIRE (assigned (sweptOpen));
    REQUIRE (assigned (sweptDisk));
    const auto removed = env.pm.sweepExpired (std::time (nullptr) + 100000);
    CHECK (std::find (removed.begin(), removed.end(), sweptOpen) != removed.end());
    CHECK (std::find (removed.begin(), removed.end(), sweptDisk) != removed.end());
    CHECK (! assigned (sweptOpen));
    CHECK (! assigned (sweptDisk));

    // the folder is empty again
    okIn (env, t, "folder.delete", { { "folder_id", folderId } });
    removeTenantDirs ({ t });
}

ROVOX_TEST (folders, bucket_unreachable_lists_from_local_cache_and_refuses_changes)
{
    auto store = std::make_shared<FakeStore>();
    const auto dir = test::makeTempDir ("folders-offline");
    const std::string t = "fold_offline";
    std::string id;

    {
        Folders online (store, true, dir);
        id = online.pf.create (t, "Survives", "").value().folder.id;
        REQUIRE (online.pf.assign (t, { newId ("p") }, id).ok());
    }

    store->offline = true;

    // a server that starts while the bucket is down lists from its local copy
    Folders restarted (store, true, dir);
    auto doc = restarted.pf.read (t);
    REQUIRE (doc.find (id) != nullptr);
    CHECK (doc.find (id)->name == "Survives");
    CHECK (doc.projects.size() == 1);

    // changes are refused (not queued)
    auto refused = restarted.pf.create (t, "Later", "");
    REQUIRE (! refused.ok());
    CHECK (refused.error().code == errc::io);
    CHECK (restarted.pf.rename (t, id, "x").error().code == errc::io);
    CHECK (restarted.pf.assign (t, { newId ("p") }, "tests").error().code == errc::io);

    // a running server whose cache has expired keeps its last copy
    store->offline = false;
    Folders running (store, true, test::makeTempDir ("folders-offline2"));
    REQUIRE (running.pf.read (t).find (id) != nullptr);
    running.pf.setCacheTtl (std::chrono::milliseconds (0));
    store->offline = true;
    CHECK (running.pf.read (t).find (id) != nullptr);

    store->offline = false;
    CHECK (restarted.pf.create (t, "Later", "").ok());
}

//==============================================================================
// E12-02: commands

ROVOX_TEST (folders, project_move_does_not_change_revision_or_undo)
{
    const std::string t = "fold_rev";
    removeTenantDirs ({ t });
    test::Env env;
    const auto id = newIn (env, t, "Steady");
    okIn (env, t, "track.create", { { "name", "Drums" } }, id);
    const auto revision = env.project (id).revision();
    const auto history = okIn (env, t, "history.list", {}, id);
    const bool dirty = env.project (id).dirty();
    const auto folderId = okIn (env, t, "folder.create", { { "name", "Somewhere" } })["folder"]["folder_id"];

    auto moved = okIn (env, t, "project.move", { { "project_id", id }, { "folder_id", folderId } });
    CHECK (moved["moved"] == 1);
    CHECK (moved["folder_id"] == folderId);

    CHECK (env.project (id).revision() == revision);
    CHECK (okIn (env, t, "history.list", {}, id) == history);
    CHECK (env.project (id).dirty() == dirty);
    CHECK (okIn (env, t, "project.catalog")["projects"][0]["folder_id"] == folderId);
    CHECK (okIn (env, t, "project.list")["projects"][0]["folder_id"] == folderId);

    // back to the top level
    moved = okIn (env, t, "project.move", { { "project_id", id }, { "folder_id", "root" } });
    CHECK (moved["folder_id"].is_null());
    CHECK (okIn (env, t, "project.catalog")["projects"][0]["folder_id"].is_null());
    CHECK (env.project (id).revision() == revision);
    removeTenantDirs ({ t });
}

ROVOX_TEST (folders, project_move_works_for_closed_and_remote_only_projects)
{
    const std::string t = "fold_remote";
    removeTenantDirs ({ t });
    auto store = std::make_shared<FakeStore>();
    auto rs = std::make_shared<RemoteStorage> (store, StorageKeys { "rovox/" }, "fake", "server-a", 60);
    rs->start();

    {
        test::Env env;
        env.pm.setStorage (rs);

        const auto closed = newIn (env, t, "Closed here");
        okIn (env, t, "project.save", {}, closed);
        const auto remote = newIn (env, t, "Only in the bucket");
        okIn (env, t, "project.save", {}, remote);
        const auto remoteDir = env.project (remote).dir();
        REQUIRE (waitFor ([&] { return rs->status (t, remote)["synced_revision"] == env.project (remote).revision(); }));
        REQUIRE (waitFor ([&] { return rs->status (t, closed)["synced_revision"] == env.project (closed).revision(); }));
        REQUIRE (env.pm.close (closed, SaveMode::save).ok());
        REQUIRE (env.pm.close (remote, SaveMode::save).ok());
        REQUIRE (rs->flush (std::chrono::seconds (10)));
        const auto headBefore = rs->fetchHead (t, remote).value().revision;

        // another server writes the remote project now (its lease is live), and this server has no copy of it
        auto other = std::make_shared<RemoteStorage> (store, StorageKeys { "rovox/" }, "fake", "server-b", 60);
        REQUIRE (waitFor ([&] { return other->acquireLease (t, remote).ok(); }));       // once this server's lease is released
        std::error_code ec;
        fs::remove_all (remoteDir.parent_path(), ec);
        REQUIRE (rs->refreshHeads().ok());

        const auto folderId = okIn (env, t, "folder.create", { { "name", "Cloud" } })["folder"]["folder_id"].get<std::string>();
        auto moved = okIn (env, t, "project.move", { { "project_ids", { closed, remote } }, { "folder_id", folderId } });
        CHECK (moved["moved"] == 2);

        auto catalog = okIn (env, t, "project.catalog", { { "folder_id", folderId } })["projects"];
        CHECK (idsOf (catalog) == (std::set<std::string> { closed, remote }));

        for (auto& p : catalog)
            if (p["project_id"] == remote)
                CHECK (p.value ("remote_only", false));

        // nothing of the project moved: same HEAD revision, the other server still holds the lease
        CHECK (rs->fetchHead (t, remote).value().revision == headBefore);
        auto lease = ProjectLease::read (*store, rs->keys().lease (t, remote));
        REQUIRE (lease.ok());
        REQUIRE (lease.value().has_value());
        CHECK (lease.value()->owner == "server-b");

        // the document is in the bucket
        auto doc = json::parse (store->objects[rs->keys().folders (t)]);
        CHECK (doc["projects"][remote] == folderId);
        CHECK (doc["projects"][closed] == folderId);
        ProjectLease::release (*store, rs->keys().lease (t, remote), "server-b");
    }

    rs->shutdown (std::chrono::seconds (5));
    removeTenantDirs ({ t });
}

ROVOX_TEST (folders, catalog_filter_folder_recursive)
{
    const std::string t = "fold_filter";
    removeTenantDirs ({ t });
    test::Env env;
    const auto top = newIn (env, t, "top"), inA = newIn (env, t, "in A"), inB = newIn (env, t, "in B"), inTests = newIn (env, t, "in tests");
    const auto a = okIn (env, t, "folder.create", { { "name", "A" } })["folder"]["folder_id"].get<std::string>();
    const auto b = okIn (env, t, "folder.create", { { "name", "B" }, { "parent_id", a } })["folder"]["folder_id"].get<std::string>();
    okIn (env, t, "project.move", { { "project_id", inA }, { "folder_id", a } });
    okIn (env, t, "project.move", { { "project_id", inB }, { "folder_id", b } });
    okIn (env, t, "project.move", { { "project_id", inTests }, { "folder_id", "tests" } });

    auto catalog = [&] (json params) { return idsOf (okIn (env, t, "project.catalog", params)["projects"]); };
    using S = std::set<std::string>;
    CHECK (catalog ({ { "folder_id", a } }) == (S { inA }));
    CHECK (catalog ({ { "folder_id", a }, { "recursive", true } }) == (S { inA, inB }));
    CHECK (catalog ({ { "folder_id", b }, { "recursive", true } }) == (S { inB }));
    CHECK (catalog ({ { "folder_id", "root" } }) == (S { top }));
    CHECK (catalog ({ { "folder_id", "root" }, { "recursive", true } }) == (S { top, inA, inB }));
    CHECK (catalog ({ { "folder_id", "root" }, { "recursive", true }, { "include_system", true } }) == (S { top, inA, inB, inTests }));
    CHECK (catalog ({ { "folder_id", "tests" } }) == (S { inTests }));
    CHECK (errorOf (runIn (env, t, "project.catalog", { { "folder_id", "fld_01JZZZZZZZZZZZZZZZZZZZZZZZ" } })) == errc::notFound);

    // project.list (open projects) takes the same filter
    CHECK (idsOf (okIn (env, t, "project.list", { { "folder_id", a }, { "recursive", true } })["projects"]) == (S { inA, inB }));
    CHECK (idsOf (okIn (env, t, "project.list", { { "folder_id", "root" } })["projects"]) == (S { top }));

    // include_folders: the folder list in the same call
    auto withFolders = okIn (env, t, "project.catalog", { { "include_folders", true } });
    REQUIRE (withFolders.contains ("folders"));
    CHECK (withFolders["folders_version"] == env.pm.folders().read (t).version);
    REQUIRE (folderNamed (withFolders["folders"], "A") != nullptr);
    CHECK ((*folderNamed (withFolders["folders"], "A"))["project_count"] == 1);
    CHECK ((*folderNamed (withFolders["folders"], "A"))["folder_count"] == 1);
    CHECK ((*folderNamed (withFolders["folders"], "B"))["parent_id"] == a);
    CHECK (withFolders["folders"].back()["folder_id"] == "tests");
    CHECK (withFolders["folders"].back()["project_count"] == 1);
    CHECK (! okIn (env, t, "project.catalog").contains ("folders"));
    removeTenantDirs ({ t });
}

ROVOX_TEST (folders, catalog_hides_tests_without_include_system)
{
    const std::string t = "fold_hide";
    removeTenantDirs ({ t });
    test::Env env;
    const auto normal = newIn (env, t, "normal"), test1 = newIn (env, t, "test track");
    okIn (env, t, "project.move", { { "project_id", test1 }, { "folder_id", "tests" } });

    // owner's decision (folders-plan §5.3): old clients calling project.catalog without params no longer see the tests
    CHECK (idsOf (okIn (env, t, "project.catalog")["projects"]) == (std::set<std::string> { normal }));
    auto all = okIn (env, t, "project.catalog", { { "include_system", true } })["projects"];
    CHECK (idsOf (all) == (std::set<std::string> { normal, test1 }));

    for (auto& p : all)
        CHECK (p["folder_id"] == (p["project_id"] == test1 ? json ("tests") : json (nullptr)));

    CHECK (idsOf (okIn (env, t, "project.catalog", { { "folder_id", "tests" } })["projects"]) == (std::set<std::string> { test1 }));

    // project.list (the open projects) is unchanged: it shows them
    CHECK (idsOf (okIn (env, t, "project.list")["projects"]) == (std::set<std::string> { normal, test1 }));
    removeTenantDirs ({ t });
}

ROVOX_TEST (folders, project_list_without_params_unchanged)
{
    const std::string t = "fold_compat";
    removeTenantDirs ({ t });
    test::Env env;
    const auto one = newIn (env, t, "one"), two = newIn (env, t, "two");
    const auto folderId = okIn (env, t, "folder.create", { { "name", "F" } })["folder"]["folder_id"];
    okIn (env, t, "project.move", { { "project_id", two }, { "folder_id", folderId } });

    auto list = okIn (env, t, "project.list")["projects"];
    CHECK (idsOf (list) == (std::set<std::string> { one, two }));

    for (auto& p : list)
    {
        for (auto* k : { "project_id", "name", "tenant_id", "revision", "dirty", "tracks", "path", "ephemeral", "ttl_s", "storage" })
            CHECK (p.contains (k));

        CHECK (p.contains ("folder_id"));
    }

    // the catalog without params: the same items as before (every project, newest first), each with folder_id added
    auto catalog = okIn (env, t, "project.catalog")["projects"];
    auto plain = env.pm.catalog (t);
    REQUIRE (catalog.size() == plain.size());

    for (size_t i = 0; i < catalog.size(); ++i)
    {
        auto withoutFolder = catalog[i];
        withoutFolder.erase ("folder_id");
        CHECK (withoutFolder == plain[i]);
    }

    removeTenantDirs ({ t });
}

ROVOX_TEST (folders, unknown_folder_or_project_not_found_all_or_nothing)
{
    const std::string t = "fold_nf";
    removeTenantDirs ({ t, "fold_nf_other" });
    test::Env env;
    const auto mine = newIn (env, t, "mine");
    const auto theirs = newIn (env, "fold_nf_other", "theirs");
    const auto folderId = okIn (env, t, "folder.create", { { "name", "Target" } })["folder"]["folder_id"].get<std::string>();
    const auto ghost = newId ("p");

    // one unknown project: nothing moves
    auto r = runIn (env, t, "project.move", { { "project_ids", { mine, ghost } }, { "folder_id", folderId } });
    CHECK (errorOf (r) == errc::notFound);
    REQUIRE (r.error.has_value());
    CHECK (r.error->details["project_ids"] == json::array ({ ghost }));
    CHECK (env.pm.folders().read (t).folderOf (mine).empty());

    // another tenant's project is unknown here
    CHECK (errorOf (runIn (env, t, "project.move", { { "project_id", theirs }, { "folder_id", folderId } })) == errc::notFound);
    CHECK (errorOf (runIn (env, t, "project.move", { { "project_id", mine }, { "folder_id", "fld_01JZZZZZZZZZZZZZZZZZZZZZZZ" } })) == errc::notFound);
    CHECK (errorOf (runIn (env, t, "project.move", { { "project_id", mine }, { "project_ids", { mine } }, { "folder_id", folderId } })) == errc::invalidParams);
    CHECK (errorOf (runIn (env, t, "project.move", { { "folder_id", folderId } })) == errc::invalidParams);
    CHECK (errorOf (runIn (env, t, "folder.rename", { { "folder_id", "fld_01JZZZZZZZZZZZZZZZZZZZZZZZ" }, { "name", "x" } })) == errc::notFound);
    CHECK (errorOf (runIn (env, t, "folder.delete", { { "folder_id", "nope" } })) == errc::notFound);
    CHECK (errorOf (runIn (env, t, "folder.create", { { "name", "x" }, { "parent_id", "fld_01JZZZZZZZZZZZZZZZZZZZZZZZ" } })) == errc::notFound);
    CHECK (errorOf (runIn (env, t, "folder.move", { { "folder_id", folderId }, { "parent_id", "tests" } })) == errc::invalidParams);
    CHECK (errorOf (runIn (env, t, "folder.rename", { { "folder_id", "tests" }, { "name", "x" } })) == errc::preconditionFailed);
    CHECK (errorOf (runIn (env, t, "folder.delete", { { "folder_id", "tests" } })) == errc::preconditionFailed);

    // a duplicate in project_ids is moved once
    CHECK (okIn (env, t, "project.move", { { "project_ids", { mine, mine } }, { "folder_id", folderId } })["moved"] == 1);

    // a non-empty folder
    auto refused = runIn (env, t, "folder.delete", { { "folder_id", folderId } });
    CHECK (errorOf (refused) == errc::preconditionFailed);
    CHECK (refused.error->details["projects"] == 1);

    // project.new into a folder that does not exist creates nothing; into one that exists puts it there
    const auto before = env.pm.catalog (t).size();
    CHECK (errorOf (runIn (env, t, "project.new", { { "name", "lost" }, { "folder_id", "fld_01JZZZZZZZZZZZZZZZZZZZZZZZ" } })) == errc::notFound);
    CHECK (env.pm.catalog (t).size() == before);
    auto made = okIn (env, t, "project.new", { { "name", "filed" }, { "folder_id", folderId } });
    CHECK (made["folder_id"] == folderId);
    CHECK (! made.contains ("warning"));
    CHECK (env.pm.folders().read (t).folderOf (made["project_id"].get<std::string>()) == folderId);
    removeTenantDirs ({ t, "fold_nf_other" });
}

ROVOX_TEST (folders, folders_changed_event_published_to_tenant_only)
{
    const std::string t = "fold_ev", other = "fold_ev_other";
    removeTenantDirs ({ t, other });
    test::Env env;
    const auto id = newIn (env, t, "evented");
    auto mine = env.hub.subscribe ({ .tenant = t });
    auto theirs = env.hub.subscribe ({ .tenant = other });

    auto nextFolders = [] (Subscription& s) -> json
    {
        Event e;

        while (s.pop (e, std::chrono::milliseconds (200)))
            if (e.body.value ("type", "") == "folders.changed")
                return e.body;

        return nullptr;
    };

    const auto created = okIn (env, t, "folder.create", { { "name", "Live" } })["folder"];
    auto ev = nextFolders (*mine);
    REQUIRE (! ev.is_null());
    CHECK (ev["change"] == "created");
    CHECK (ev["folder_id"] == created["folder_id"]);
    CHECK (ev["version"] == env.pm.folders().read (t).version);
    CHECK (ev["tenant_id"] == t);

    okIn (env, t, "project.move", { { "project_id", id }, { "folder_id", created["folder_id"] } });
    ev = nextFolders (*mine);
    REQUIRE (! ev.is_null());
    CHECK (ev["change"] == "projects_moved");
    CHECK (ev["project_ids"] == json::array ({ id }));
    CHECK (ev["folder_id"] == created["folder_id"]);

    okIn (env, t, "folder.rename", { { "folder_id", created["folder_id"] }, { "name", "Renamed" } });
    CHECK (nextFolders (*mine)["change"] == "renamed");
    okIn (env, t, "project.move", { { "project_id", id }, { "folder_id", "root" } });
    ev = nextFolders (*mine);
    CHECK (ev["folder_id"].is_null());
    okIn (env, t, "folder.delete", { { "folder_id", created["folder_id"] } });
    CHECK (nextFolders (*mine)["change"] == "deleted");

    // a failed change publishes nothing; the other tenant saw none of it
    runIn (env, t, "folder.delete", { { "folder_id", "tests" } });
    CHECK (nextFolders (*mine).is_null());
    CHECK (nextFolders (*theirs).is_null());
    removeTenantDirs ({ t, other });
}
