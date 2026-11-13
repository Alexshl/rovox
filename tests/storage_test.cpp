// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Object storage as the source of truth: key layout, HEAD, the writer lease, push order (media, then snapshot, then HEAD), pull into an empty cache,
// background upload with retries, the S3 client against a local mock server (SigV4, path-style, retries, list, multipart) and a project that survives
// the loss of its cache. All against an in-memory fake store or a local HTTP server: no real network.
#include "FsObjectStore.h"
#include "Hmac.h"
#include "Ids.h"
#include "RemoteStorage.h"
#include "Sha256.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <httplib.h>

#include <atomic>
#include <fstream>
#include <set>
#include <thread>

#include <stdlib.h>

using namespace rovox;

namespace
{
/** An in-memory bucket. Thread-safe; can be told to fail the next puts. */
class FakeStore : public ObjectStore
{
public:
    Result<ObjectMeta> head (const std::string& key) override
    {
        if (headDelayMs > 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (headDelayMs));

        std::lock_guard<std::mutex> lock (mutex);
        auto it = objects.find (key);

        if (it == objects.end())
            return makeError (errc::notFound, "no " + key);

        return ObjectMeta { key, etagOf (key), it->second.size() };
    }

    /** Test helper: writes an object behind the store's back (as another server would), bumping its ETag. */
    void setObject (const std::string& key, const std::string& body)
    {
        std::lock_guard<std::mutex> lock (mutex);
        objects[key] = body;
        versions[key] = nextVersion++;
    }

    Result<std::string> get (const std::string& key) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        auto it = objects.find (key);

        if (it == objects.end())
            return makeError (errc::notFound, "no " + key);

        return it->second;
    }

    Result<void> getFile (const std::string& key, const fs::path& dest) override
    {
        auto body = get (key);

        if (! body.ok())
            return body.error();

        std::error_code ec;
        fs::create_directories (dest.parent_path(), ec);
        std::ofstream (dest, std::ios::binary) << body.value();
        return {};
    }

    Result<void> put (const std::string& key, const std::string& body, const std::string&) override
    {
        return doPut (key, body, nullptr);
    }

    /** Honours the preconditions like S3 (412 = putConditionFailed) unless `honourConditions` is off (then it behaves like a store that ignores them). */
    Result<void> putIf (const std::string& key, const std::string& body, const std::string&, const PutCondition& cond) override
    {
        return doPut (key, body, &cond);
    }

    /** A multipart-like upload: `fileParts` parts of `partDelayMs` each, polling `cancel` between them. */
    Result<void> putFileCancellable (const std::string& key, const fs::path& file, const std::string& type, const CancelCheck& cancel) override
    {
        for (int i = 0; i < fileParts; ++i)
        {
            if (i > 0 && cancel && cancel())
            {
                cancelledUploads++;
                return makeError (errc::cancelled, "cancelled between parts");
            }

            if (partDelayMs > 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (partDelayMs));
        }

        return putFile (key, file, type);
    }

    Result<void> putFile (const std::string& key, const fs::path& file, const std::string& type) override
    {
        std::ifstream in (file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return put (key, ss.str(), type);
    }

    Result<ListResult> list (const std::string& prefix, const std::string& delimiter) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        ListResult out;
        std::set<std::string> folded;

        for (auto& [k, v] : objects)
        {
            if (k.rfind (prefix, 0) != 0)
                continue;

            const auto rest = k.substr (prefix.size());

            if (! delimiter.empty() && rest.find (delimiter) != std::string::npos)
                folded.insert (prefix + rest.substr (0, rest.find (delimiter) + 1));
            else
                out.objects.push_back ({ k, etagOf (k), v.size() });
        }

        out.prefixes.assign (folded.begin(), folded.end());
        return out;
    }

    Result<void> remove (const std::string& key) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        objects.erase (key);
        return {};
    }

    std::string describe() const override   { return "fake"; }

    size_t countPuts (const std::string& containing)
    {
        std::lock_guard<std::mutex> lock (mutex);
        return (size_t) std::count_if (putLog.begin(), putLog.end(), [&] (const std::string& k) { return k.find (containing) != std::string::npos; });
    }

    std::mutex mutex;
    std::map<std::string, std::string> objects;
    std::map<std::string, int> versions;
    std::vector<std::string> putLog;
    int failPuts = 0, nextVersion = 1;
    bool honourConditions = true;
    std::string slowKey;                    // puts of keys containing this take slowPutMs
    std::atomic<int> slowPutMs { 0 }, headDelayMs { 0 }, fileParts { 1 }, partDelayMs { 0 }, cancelledUploads { 0 };

private:
    std::string etagOf (const std::string& key) const      // call with the mutex held
    {
        auto it = versions.find (key);
        return "\"v" + std::to_string (it == versions.end() ? 0 : it->second) + "\"";
    }

    Result<void> doPut (const std::string& key, const std::string& body, const PutCondition* cond)
    {
        if (slowPutMs > 0 && ! slowKey.empty() && key.find (slowKey) != std::string::npos)
            std::this_thread::sleep_for (std::chrono::milliseconds (slowPutMs));

        std::lock_guard<std::mutex> lock (mutex);

        if (failPuts > 0)
        {
            --failPuts;
            return makeError (errc::busy, "injected failure");
        }

        if (cond != nullptr && honourConditions)
        {
            const bool exists = objects.count (key) > 0;

            if (! cond->ifNoneMatch.empty() && exists)
                return makeError (putConditionFailed, "412: " + key + " exists");

            if (! cond->ifMatch.empty() && (! exists || etagOf (key) != cond->ifMatch))
                return makeError (putConditionFailed, "412: " + key + " changed");
        }

        objects[key] = body;
        versions[key] = nextVersion++;
        putLog.push_back (key);
        return {};
    }
};

const std::string shaA (64, 'a');

struct Cache
{
    fs::path dir, root;
    std::string id, tenant;
};

/** A project directory as the app writes it: edit.xml, rovox.json (media listed) and one media file. */
Cache makeCache (const std::string& tenant, std::uint64_t revision, const std::string& name = "Song")
{
    Cache c;
    c.root = test::makeTempDir ("rs");
    c.dir = c.root / "project.rvx";
    c.id = newId ("p");
    c.tenant = tenant;
    fs::create_directories (c.dir / "media");
    std::ofstream (c.dir / "media" / (shaA + ".wav"), std::ios::binary) << "RIFFfakewave";
    std::ofstream (c.dir / "edit.xml") << "<EDIT rovox:revision=\"" << revision << "\"/>";
    json media = json::array ({ { { "sha256", shaA }, { "path", "media/" + shaA + ".wav" } }, { { "sha256", "ext" }, { "path", "/abs/x.wav" }, { "external", true } } });
    std::ofstream (c.dir / "rovox.json") << json { { "project_id", c.id }, { "tenant_id", tenant }, { "name", name }, { "revision", revision }, { "media", media } }.dump();
    return c;
}

SnapshotJob jobFor (const Cache& c)
{
    ProjectHead head;
    head.tracks = 3;
    head.durationS = 12.5;
    auto job = readSnapshot (c.dir, c.dir, head);
    REQUIRE (job.ok());
    return job.value();
}

std::shared_ptr<RemoteStorage> makeStorage (const std::shared_ptr<FakeStore>& store, const std::string& owner, const std::string& prefix = "rovox/", int leaseTtlS = 60)
{
    return std::make_shared<RemoteStorage> (store, StorageKeys { prefix }, "fake", owner, leaseTtlS);
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

void clearEnv()
{
    for (const char* n : { "S3_ENDPOINT", "S3_REGION", "S3_BUCKET", "S3_ACCESS_KEY", "S3_SECRET_KEY", "S3_PREFIX", "S3_ACCESS_KEY_FILE", "S3_SECRET_KEY_FILE", "ROVOX_STORAGE", "S3_PATH_STYLE", "ROVOX_LEASE_TTL_S" })
        ::unsetenv (n);
}
} // namespace

ROVOX_TEST (storage, key_layout)
{
    StorageKeys k { StorageKeys::normalisePrefix ("/rovox") };
    CHECK (k.prefix == "rovox/");
    CHECK (StorageKeys::normalisePrefix ("") == "");
    CHECK (k.revFile ("local", "p_1", 7, "edit.xml") == "rovox/tenants/local/projects/p_1/rev/7/edit.xml");
    CHECK (k.head ("acme", "p_1") == "rovox/tenants/acme/projects/p_1/HEAD");
    CHECK (k.lease ("acme", "p_1") == "rovox/tenants/acme/projects/p_1/LEASE");
    CHECK (k.render ("acme", "p_1", "mix.mp3") == "rovox/tenants/acme/projects/p_1/renders/mix.mp3");
    CHECK (k.media ("acme", shaA, ".wav") == "rovox/tenants/acme/media/sha256/aa/" + shaA + ".wav");
}

ROVOX_TEST (storage, config_comes_from_the_environment_and_secret_files)
{
    clearEnv();
    auto local = StorageConfig::fromEnvironment();
    REQUIRE (local.ok());
    CHECK (! local.value().s3);                                  // no S3 variables: local

    ::setenv ("S3_ENDPOINT", "https://fsn1.your-objectstorage.com", 1);
    ::setenv ("S3_BUCKET", "studio", 1);
    auto partial = StorageConfig::fromEnvironment();
    REQUIRE (! partial.ok());                                    // endpoint + bucket select s3, the keys are missing
    CHECK (partial.error().message.find ("S3_ACCESS_KEY") != std::string::npos);
    CHECK (partial.error().message.find ("S3_ENDPOINT") == std::string::npos);

    const auto secrets = test::makeTempDir ("s3secret");
    std::ofstream (secrets / "secret") << "  super-secret-value \n";
    ::setenv ("S3_ACCESS_KEY", "AKIAEXAMPLE", 1);
    ::setenv ("S3_SECRET_KEY_FILE", (secrets / "secret").c_str(), 1);
    ::setenv ("S3_REGION", "fsn1", 1);
    ::setenv ("S3_PREFIX", "tenant-a", 1);
    auto s3 = StorageConfig::fromEnvironment();
    REQUIRE (s3.ok());
    CHECK (s3.value().s3);
    CHECK (s3.value().store.secretKey == "super-secret-value");
    CHECK (s3.value().store.region == "fsn1");
    CHECK (s3.value().store.pathStyle);
    CHECK (s3.value().prefix == "tenant-a/");

    const auto shown = s3.value().describe().dump();
    CHECK (shown.find ("super-secret-value") == std::string::npos);
    CHECK (shown.find ("AKIAEXAMPLE") == std::string::npos);
    CHECK (shown.find ("studio") != std::string::npos);

    CHECK (StorageConfig::fromEnvironment ("local").value().s3 == false);      // --storage local wins over the variables
    CHECK (! StorageConfig::fromEnvironment ("ftp").ok());
    ::setenv ("ROVOX_STORAGE", "local", 1);
    CHECK (! StorageConfig::fromEnvironment().value().s3);
    CHECK (StorageConfig::fromEnvironment ("s3").value().s3);
    clearEnv();
}

ROVOX_TEST (storage, head_object_round_trips)
{
    ProjectHead h;
    h.projectId = "p_x";
    h.name = "Рахів";
    h.revision = 42;
    h.modified = 1700000000;
    h.tracks = 5;
    h.durationS = 61.25;
    const auto back = ProjectHead::fromJson (json::parse (h.toJson().dump()), "local", "p_x");
    REQUIRE (back.has_value());
    CHECK (back->name == "Рахів" && back->revision == 42 && back->tracks == 5 && back->modified == 1700000000 && back->durationS == 61.25);
    CHECK (! ProjectHead::fromJson (json { { "name", "x" } }, "local", "p_x").has_value());
}

ROVOX_TEST (storage, lease_allows_one_live_writer)
{
    FakeStore store;
    std::int64_t now = 1000;
    const ProjectLease::Clock clock = [&] { return now; };
    const std::string key = "rovox/tenants/local/projects/p_1/LEASE";

    REQUIRE (ProjectLease::acquire (store, key, "server-a", 60, clock).ok());
    CHECK (ProjectLease::acquire (store, key, "server-a", 60, clock).ok());         // re-entrant for the owner

    auto other = ProjectLease::acquire (store, key, "server-b", 60, clock);
    REQUIRE (! other.ok());
    CHECK (other.error().code == errc::busy);
    CHECK (other.error().details.value ("lease_owner", "") == "server-a");
    CHECK (other.error().message.find ("server-a") != std::string::npos);

    now = 1030;
    CHECK (ProjectLease::renew (store, key, "server-a", 60, clock).ok());
    CHECK (ProjectLease::read (store, key).value()->expires == 1090);

    now = 1100;                                                                      // expired without a renewal: anybody may take over
    CHECK (ProjectLease::acquire (store, key, "server-b", 60, clock).ok());
    auto lost = ProjectLease::renew (store, key, "server-a", 60, clock);
    REQUIRE (! lost.ok());                                                           // the old owner learns it lost the project
    CHECK (lost.error().details.contains ("lease_owner"));

    ProjectLease::release (store, key, "server-a");                                  // not the owner: no effect
    CHECK (ProjectLease::read (store, key).value().has_value());
    ProjectLease::release (store, key, "server-b");
    CHECK (! ProjectLease::read (store, key).value().has_value());
}

ROVOX_TEST (storage, push_uploads_media_then_snapshot_then_head_and_is_idempotent)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "a");
    auto c = makeCache ("local", 4);
    const auto job = jobFor (c);
    CHECK (job.media.size() == 1);                              // the external media entry is not uploaded

    REQUIRE (rs->push (job).ok());
    const auto mediaKey = rs->keys().media ("local", shaA, ".wav");
    const auto editKey = rs->keys().revFile ("local", c.id, 4, "edit.xml"), manifestKey = rs->keys().revFile ("local", c.id, 4, "rovox.json"), headKey = rs->keys().head ("local", c.id);
    CHECK (store->putLog == std::vector<std::string> ({ mediaKey, editKey, manifestKey, headKey }));        // HEAD moves last

    auto head = rs->fetchHead ("local", c.id);
    REQUIRE (head.ok());
    CHECK (head.value().revision == 4 && head.value().tracks == 3 && head.value().name == "Song" && head.value().durationS == 12.5);

    REQUIRE (rs->push (job).ok());                              // same revision again: nothing is written
    CHECK (store->putLog.size() == 4);

    // a new revision uploads only the new snapshot (the media is known)
    std::ofstream (c.dir / "rovox.json") << json { { "project_id", c.id }, { "tenant_id", "local" }, { "name", "Song v2" }, { "revision", 5 },
                                                   { "media", json::array ({ { { "sha256", shaA }, { "path", "media/" + shaA + ".wav" } } }) } }.dump();
    REQUIRE (rs->push (jobFor (c)).ok());
    CHECK (store->countPuts ("/media/") == 1);
    CHECK (rs->fetchHead ("local", c.id).value().revision == 5);
    CHECK (store->objects.count (rs->keys().revFile ("local", c.id, 4, "edit.xml")) == 1);       // old snapshots stay
}

ROVOX_TEST (storage, push_never_moves_head_backwards)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "a");
    auto c = makeCache ("local", 3);
    ProjectHead ahead;
    ahead.projectId = c.id;
    ahead.revision = 9;
    store->objects[rs->keys().head ("local", c.id)] = ahead.toJson().dump();

    auto r = rs->push (jobFor (c));
    REQUIRE (! r.ok());
    CHECK (r.error().code == errc::revisionConflict);
    CHECK (rs->fetchHead ("local", c.id).value().revision == 9);
    CHECK (store->countPuts ("/rev/") == 0);
}

ROVOX_TEST (storage, pull_rebuilds_an_empty_cache)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "a");
    auto c = makeCache ("local", 6);
    REQUIRE (rs->push (jobFor (c)).ok());

    auto fresh = makeStorage (store, "b");
    const auto target = test::makeTempDir ("pull") / "project.rvx";
    auto rev = fresh->pull ("local", c.id, target);
    REQUIRE (rev.ok());
    CHECK (rev.value() == 6);

    auto read = [] (const fs::path& f) { std::ifstream in (f, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
    CHECK (read (target / "edit.xml") == read (c.dir / "edit.xml"));
    CHECK (read (target / "rovox.json") == read (c.dir / "rovox.json"));
    CHECK (read (target / "media" / (shaA + ".wav")) == "RIFFfakewave");
    CHECK (fresh->status ("local", c.id)["synced_revision"] == 6);
}

ROVOX_TEST (storage, pull_ignores_media_paths_that_leave_the_project)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "a");
    const auto id = newId ("p");
    ProjectHead h;
    h.projectId = id;
    h.revision = 1;
    store->objects[rs->keys().head ("local", id)] = h.toJson().dump();
    store->objects[rs->keys().revFile ("local", id, 1, "edit.xml")] = "<EDIT/>";
    store->objects[rs->keys().revFile ("local", id, 1, "rovox.json")] =
        json { { "project_id", id }, { "tenant_id", "local" }, { "revision", 1 },
               { "media", json::array ({ { { "sha256", shaA }, { "path", "media/../../evil.wav" } }, { { "sha256", shaA }, { "path", "/etc/passwd" } }, { { "sha256", "short" }, { "path", "media/short.wav" } } }) } }.dump();
    store->objects[rs->keys().media ("local", shaA, ".wav")] = "x";

    const auto root = test::makeTempDir ("evil");
    REQUIRE (rs->pull ("local", id, root / "a" / "project.rvx").ok());
    CHECK (! fs::exists (root / "evil.wav"));
    CHECK (! fs::exists (root / "a" / "evil.wav"));
    CHECK (fs::is_empty (root / "a" / "project.rvx" / "media"));

    CHECK (! rs->pull ("../x", id, root / "b").ok());                                // ids are validated
    CHECK (! rs->pull ("local", "p_../../x", root / "b").ok());
}

ROVOX_TEST (storage, background_upload_retries_and_reports_status)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "a");
    rs->setRetryBackoff (std::chrono::milliseconds (5));
    auto c = makeCache ("local", 2);

    std::mutex m;
    std::vector<json> events;
    rs->setListener ([&] (const std::string&, const std::string&, const json& st) { std::lock_guard<std::mutex> lock (m); events.push_back (st); });

    rs->enqueue (jobFor (c));                                   // not started yet: queued
    CHECK (rs->pendingUploads() == 1);
    CHECK (rs->status ("local", c.id)["pending_uploads"] == 1);
    CHECK (rs->status ("local", c.id)["synced_revision"].is_null());

    store->failPuts = 3;
    rs->start();
    REQUIRE (waitFor ([&] { return rs->status ("local", c.id)["synced_revision"] == 2; }));
    const auto st = rs->status ("local", c.id);
    CHECK (st["pending_uploads"] == 0);
    CHECK (st["last_error"].is_null());
    CHECK (st["backend"] == "s3");

    {
        std::lock_guard<std::mutex> lock (m);
        const bool sawError = std::any_of (events.begin(), events.end(), [] (const json& e) { return e["last_error"].is_string(); });
        CHECK (sawError);                                       // the failed attempts were visible while they lasted
    }

    CHECK (rs->shutdown (std::chrono::seconds (5)));
}

ROVOX_TEST (storage, latest_snapshot_of_a_project_wins_and_shutdown_flushes)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "a");
    auto c = makeCache ("local", 1);
    rs->enqueue (jobFor (c));
    std::ofstream (c.dir / "rovox.json") << json { { "project_id", c.id }, { "tenant_id", "local" }, { "name", "Song" }, { "revision", 2 }, { "media", json::array() } }.dump();
    rs->enqueue (jobFor (c));
    CHECK (rs->pendingUploads() == 1);

    rs->start();
    CHECK (rs->shutdown (std::chrono::seconds (10)));
    CHECK (rs->status ("local", c.id)["synced_revision"] == 2);
    CHECK (store->objects.count (rs->keys().revFile ("local", c.id, 1, "rovox.json")) == 0);
}

ROVOX_TEST (storage, lease_released_after_the_pending_snapshot_and_taken_over_by_another_server)
{
    auto store = std::make_shared<FakeStore>();
    auto a = makeStorage (store, "server-a"), b = makeStorage (store, "server-b");
    auto c = makeCache ("local", 1);
    a->start();
    REQUIRE (a->acquireLease ("local", c.id).ok());

    auto refused = b->acquireLease ("local", c.id);
    REQUIRE (! refused.ok());
    CHECK (refused.error().details.value ("lease_owner", "") == "server-a");

    a->enqueue (jobFor (c));
    a->releaseLease ("local", c.id);
    CHECK (a->flush (std::chrono::seconds (10)));
    CHECK (a->status ("local", c.id)["synced_revision"] == 1);
    CHECK (b->acquireLease ("local", c.id).ok());               // released only after the snapshot went up
    a->shutdown (std::chrono::seconds (2));
}

ROVOX_TEST (storage, conditional_lease_take_over_and_renewal_use_etags)
{
    FakeStore store;
    std::int64_t now = 1000;
    const ProjectLease::Clock clock = [&] { return now; };
    const std::string key = "rovox/tenants/local/projects/p_1/LEASE";

    REQUIRE (ProjectLease::acquire (store, key, "server-a", 60, clock, true).ok());
    CHECK (! ProjectLease::acquire (store, key, "server-b", 60, clock, true).ok());
    now = 1030;
    REQUIRE (ProjectLease::renew (store, key, "server-a", 60, clock, true).ok());
    CHECK (ProjectLease::read (store, key).value()->expires == 1090);

    now = 1100;                                                 // expired: take-over with If-Match
    REQUIRE (ProjectLease::acquire (store, key, "server-b", 60, clock, true).ok());
    auto lost = ProjectLease::renew (store, key, "server-a", 60, clock, true);
    REQUIRE (! lost.ok());
    CHECK (lost.error().details.contains ("lease_owner"));
}

ROVOX_TEST (storage, lease_is_renewed_during_a_push_longer_than_the_ttl)
{
    auto store = std::make_shared<FakeStore>();
    store->fileParts = 16;                                      // a 4 s upload against a 3 s lease
    store->partDelayMs = 250;
    auto a = makeStorage (store, "server-a", "rovox/", 3), b = makeStorage (store, "server-b", "rovox/", 3);
    a->setIntervals (std::chrono::milliseconds (60000), std::chrono::milliseconds (200));
    auto c = makeCache ("local", 1);
    a->start();
    REQUIRE (a->acquireLease ("local", c.id).ok());
    a->enqueue (jobFor (c));

    const auto begin = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] { return (int) std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - begin).count(); };
    int refusedAfterTtl = 0;
    bool tookOver = false;

    while (a->status ("local", c.id)["synced_revision"].is_null() && elapsedMs() < 20000)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (150));

        if (elapsedMs() > 3500)                                 // the original lease ran out long ago unless it was renewed
        {
            if (b->acquireLease ("local", c.id).ok())
                tookOver = true;
            else
                ++refusedAfterTtl;
        }
    }

    CHECK (! tookOver);
    CHECK (refusedAfterTtl > 0);
    CHECK (a->status ("local", c.id)["synced_revision"] == 1);
    CHECK (a->status ("local", c.id)["lease"] == "held");
    CHECK (store->objects.count (a->keys().head ("local", c.id)) == 1);
    a->shutdown (std::chrono::seconds (5));
}

ROVOX_TEST (storage, push_does_not_write_head_after_the_lease_was_taken_over)
{
    auto store = std::make_shared<FakeStore>();
    store->fileParts = 10;                                      // a 2 s upload
    store->partDelayMs = 200;
    auto a = makeStorage (store, "server-a", "rovox/", 3);
    a->setIntervals (std::chrono::milliseconds (60000), std::chrono::milliseconds (100));
    a->setRetryBackoff (std::chrono::milliseconds (5000));
    auto c = makeCache ("local", 1);
    a->start();
    REQUIRE (a->acquireLease ("local", c.id).ok());
    a->enqueue (jobFor (c));
    std::this_thread::sleep_for (std::chrono::milliseconds (300));

    store->setObject (a->keys().lease ("local", c.id), json { { "owner", "server-b" }, { "expires", ProjectLease::systemNow() + 60 }, { "acquired", ProjectLease::systemNow() } }.dump());

    REQUIRE (waitFor ([&] { return a->status ("local", c.id)["lease"] == "lost"; }, 20000));
    REQUIRE (waitFor ([&] { return store->cancelledUploads >= 1; }, 20000));          // the upload in flight noticed at a part boundary
    CHECK (a->status ("local", c.id)["last_error"].get<std::string>().find ("lease") != std::string::npos);

    CHECK (store->objects.count (a->keys().head ("local", c.id)) == 0);
    CHECK (store->objects.count (a->keys().revFile ("local", c.id, 1, "rovox.json")) == 0);
    CHECK (store->cancelledUploads >= 1);                       // the upload stopped at a part boundary
    CHECK (a->status ("local", c.id)["synced_revision"].is_null());
    a->shutdown (std::chrono::seconds (2));
    CHECK (ProjectLease::read (*store, a->keys().lease ("local", c.id)).value()->owner == "server-b");       // the new owner's lease is not released by us
}

ROVOX_TEST (storage, two_concurrent_acquirers_exactly_one_wins_with_conditional_writes)
{
    for (int round = 0; round < 5; ++round)
    {
        auto store = std::make_shared<FakeStore>();
        store->headDelayMs = 30;                                // both read "no lease" before either writes
        auto b = makeStorage (store, "server-b"), c = makeStorage (store, "server-c");
        REQUIRE (b->conditionalWritesActive());                 // probe up front so only the acquisition races
        REQUIRE (c->conditionalWritesActive());

        std::atomic<bool> go { false };
        std::atomic<int> winners { 0 }, busy { 0 };
        const auto race = [&] (const std::shared_ptr<RemoteStorage>& rs)
        {
            while (! go)
                std::this_thread::yield();

            auto r = rs->acquireLease ("local", "p_race");

            if (r.ok())
                ++winners;
            else if (r.error().code == errc::busy && r.error().details.contains ("lease_owner"))
                ++busy;
        };
        std::thread t1 ([&] { race (b); }), t2 ([&] { race (c); });
        go = true;
        t1.join();
        t2.join();
        CHECK (winners == 1);
        CHECK (busy == 1);
    }
}

ROVOX_TEST (storage, probe_detects_a_store_that_ignores_preconditions_and_leases_fall_back)
{
    auto store = std::make_shared<FakeStore>();
    store->honourConditions = false;
    auto a = makeStorage (store, "server-a"), b = makeStorage (store, "server-b");
    CHECK (! a->conditionalWritesActive());
    const auto d = a->describe();
    CHECK (d.value ("conditional_writes", "") == "fallback");
    CHECK (d.value ("warning", "").find ("best-effort") != std::string::npos);
    CHECK (std::none_of (store->objects.begin(), store->objects.end(), [] (const auto& kv) { return kv.first.find ("probe/") != std::string::npos; }));      // scratch key cleaned up

    REQUIRE (a->acquireLease ("local", "p_fb").ok());               // the best-effort write + read-back path still works
    auto refused = b->acquireLease ("local", "p_fb");
    REQUIRE (! refused.ok());
    CHECK (refused.error().details.value ("lease_owner", "") == "server-a");

    auto honest = std::make_shared<FakeStore>();
    auto c = makeStorage (honest, "server-c");
    CHECK (c->conditionalWritesActive());
    CHECK (c->describe().value ("conditional_writes", "") == "on");
    CHECK (! c->describe().contains ("warning"));

    auto forcedOn = makeStorage (store, "server-d"), forcedOff = makeStorage (honest, "server-e");
    forcedOn->setConditionalWrites ("on");
    forcedOff->setConditionalWrites ("off");
    CHECK (forcedOn->conditionalWritesActive());                    // no probe: trusted
    CHECK (! forcedOff->conditionalWritesActive());
}

ROVOX_TEST (storage, shutdown_during_a_slow_push_returns_within_the_bound)
{
    auto store = std::make_shared<FakeStore>();
    store->fileParts = 50;                                      // a 10 s upload
    store->partDelayMs = 200;
    auto a = makeStorage (store, "server-a");
    auto c = makeCache ("local", 1);
    a->start();
    REQUIRE (a->acquireLease ("local", c.id).ok());
    a->enqueue (jobFor (c));
    std::this_thread::sleep_for (std::chrono::milliseconds (400));

    const auto begin = std::chrono::steady_clock::now();
    const bool clean = a->shutdown (std::chrono::seconds (1));
    const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - begin).count();

    CHECK (! clean);                                            // the snapshot did not make it
    CHECK (tookMs < 4000);                                      // 1 s of grace + the part in flight, not the whole upload
    CHECK (store->cancelledUploads == 1);
    CHECK (store->objects.count (a->keys().head ("local", c.id)) == 0);
}

ROVOX_TEST (storage, heads_listing_feeds_the_catalog_and_forget_removes_a_project)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "a");
    auto one = makeCache ("local", 2, "First"), two = makeCache ("acme", 7, "Second");
    REQUIRE (rs->push (jobFor (one)).ok());
    REQUIRE (rs->push (jobFor (two)).ok());

    auto other = makeStorage (store, "b");
    REQUIRE (other->refreshHeads().ok());
    REQUIRE (other->heads ("local").size() == 1);
    CHECK (other->heads ("local")[0].name == "First");
    CHECK (other->heads ("acme")[0].revision == 7);
    CHECK (other->findByName ("local", "First")->projectId == one.id);
    CHECK (! other->findByName ("local", "Second").has_value());                // tenants are separate

    other->start();
    other->forget ("local", one.id);
    CHECK (other->flush (std::chrono::seconds (10)));
    CHECK (store->objects.count (other->keys().head ("local", one.id)) == 0);
    CHECK (store->objects.count (other->keys().revFile ("local", one.id, 2, "rovox.json")) == 1);   // snapshots stay
    other->shutdown (std::chrono::seconds (2));
}

ROVOX_TEST (storage, renders_go_up_with_their_sidecar_and_come_back)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "a");
    const auto dir = test::makeTempDir ("rend");
    std::ofstream (dir / "mix.mp3") << "mp3data";
    std::ofstream (dir / "mix.render.json") << "{}";
    auto key = rs->uploadRender ("local", "p_1", dir / "mix.mp3");
    REQUIRE (key.ok());
    CHECK (key.value() == "rovox/tenants/local/projects/p_1/renders/mix.mp3");
    CHECK (store->objects.count ("rovox/tenants/local/projects/p_1/renders/mix.render.json") == 1);

    CHECK (rs->fetchRender ("local", "p_1", "mix.mp3", dir / "back" / "mix.mp3").ok());
    CHECK (fs::file_size (dir / "back" / "mix.mp3") == 7);
    CHECK (! rs->fetchRender ("local", "p_1", "../mix.mp3", dir / "x").ok());

    fs::create_directories (dir.parent_path() / "renders");
    const auto project = test::makeTempDir ("rendproj");
    fs::create_directories (project / "renders");
    std::ofstream (project / "renders" / "a.wav") << "wav";
    std::ofstream (project / "renders" / ".hidden.part") << "x";
    auto pushed = rs->pushRenders ("local", "p_2", project);
    REQUIRE (pushed.ok());
    CHECK (pushed.value() == 1);
    CHECK (rs->pushRenders ("local", "p_2", project).value() == 0);              // idempotent
}

//==============================================================================
namespace
{
/** A tiny S3: path-style /bkt/<key>, checks that requests are signed, can answer 503 first. */
struct MockS3
{
    MockS3()
    {
        auto guard = [this] (const httplib::Request& req, httplib::Response& res)
        {
            std::lock_guard<std::mutex> lock (mutex);
            ++requests;
            lastAuth = req.get_header_value ("Authorization");
            lastHost = req.get_header_value ("Host");
            lastPath = req.path;
            lastPayloadHash = req.get_header_value ("x-amz-content-sha256");

            if (failNext > 0)
            {
                --failNext;
                res.status = 503;
                return false;
            }

            return true;
        };

        svr.Put (R"(/bkt/(.+))", [this, guard] (const httplib::Request& req, httplib::Response& res)
        {
            if (! guard (req, res))
                return;

            std::lock_guard<std::mutex> lock (mutex);

            if (req.has_param ("partNumber"))
            {
                parts[req.get_param_value ("uploadId")][std::stoi (req.get_param_value ("partNumber"))] = req.body;
                res.set_header ("ETag", "\"part" + req.get_param_value ("partNumber") + "\"");
                return;
            }

            // preconditions like S3: If-None-Match: * only when the key is new, If-Match only against the current ETag (always "etag" here)
            const bool exists = objects.count (req.matches[1]) > 0;
            lastIfNoneMatch = req.get_header_value ("If-None-Match");
            lastIfMatch = req.get_header_value ("If-Match");

            if ((lastIfNoneMatch == "*" && exists) || (! lastIfMatch.empty() && (! exists || lastIfMatch != "\"etag\"")))
            {
                res.status = 412;
                res.set_content ("<Error><Code>PreconditionFailed</Code><Message>at least one precondition failed</Message></Error>", "application/xml");
                return;
            }

            objects[req.matches[1]] = req.body;
            res.set_header ("ETag", "\"etag\"");
        });

        svr.Post (R"(/bkt/(.+))", [this, guard] (const httplib::Request& req, httplib::Response& res)
        {
            if (! guard (req, res))
                return;

            std::lock_guard<std::mutex> lock (mutex);

            if (req.has_param ("uploads"))
            {
                res.set_content ("<InitiateMultipartUploadResult><UploadId>up1</UploadId></InitiateMultipartUploadResult>", "application/xml");
                return;
            }

            completeBody = req.body;
            std::string all;

            for (auto& [n, body] : parts["up1"])
                all += body;

            objects[req.matches[1]] = all;
            res.set_content ("<CompleteMultipartUploadResult/>", "application/xml");
        });

        svr.Get (R"(/bkt/(.+))", [this, guard] (const httplib::Request& req, httplib::Response& res)
        {
            if (! guard (req, res))
                return;

            std::lock_guard<std::mutex> lock (mutex);
            auto it = objects.find (req.matches[1]);

            if (it == objects.end())
            {
                res.status = 404;
                res.set_content ("<Error><Code>NoSuchKey</Code><Message>nope</Message></Error>", "application/xml");
                return;
            }

            res.set_header ("ETag", "\"etag\"");
            res.set_content (it->second, "application/octet-stream");
        });

        svr.Get (R"(/bkt)", [this, guard] (const httplib::Request& req, httplib::Response& res)
        {
            if (! guard (req, res))
                return;

            std::lock_guard<std::mutex> lock (mutex);
            const auto prefix = req.get_param_value ("prefix"), delimiter = req.get_param_value ("delimiter");
            const auto token = req.get_param_value ("continuation-token");
            std::vector<std::string> keys;
            std::set<std::string> folded;

            for (auto& [k, v] : objects)
                if (k.rfind (prefix, 0) == 0)
                {
                    const auto rest = k.substr (prefix.size());

                    if (! delimiter.empty() && rest.find (delimiter) != std::string::npos)
                        folded.insert (prefix + rest.substr (0, rest.find (delimiter) + 1));
                    else
                        keys.push_back (k);
                }

            // two keys per page to exercise the continuation token
            const size_t start = token.empty() ? 0 : (size_t) std::stoi (token);
            std::string xml = "<ListBucketResult>";

            for (size_t i = start; i < std::min (keys.size(), start + 2); ++i)
                xml += "<Contents><Key>" + keys[i] + "</Key><Size>" + std::to_string (objects[keys[i]].size()) + "</Size><ETag>&quot;e&quot;</ETag></Contents>";

            for (auto& f : folded)
                xml += "<CommonPrefixes><Prefix>" + f + "</Prefix></CommonPrefixes>";

            const bool more = start + 2 < keys.size();
            xml += std::string ("<IsTruncated>") + (more ? "true" : "false") + "</IsTruncated>";

            if (more)
                xml += "<NextContinuationToken>" + std::to_string (start + 2) + "</NextContinuationToken>";

            res.set_content (xml + "</ListBucketResult>", "application/xml");
        });

        svr.Delete (R"(/bkt/(.+))", [this, guard] (const httplib::Request& req, httplib::Response& res)
        {
            if (! guard (req, res))
                return;

            std::lock_guard<std::mutex> lock (mutex);
            objects.erase (req.matches[1]);
            res.status = 204;
        });

        port = svr.bind_to_any_port ("127.0.0.1");
        thread = std::thread ([this] { svr.listen_after_bind(); });

        for (int i = 0; i < 200 && ! svr.is_running(); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    ~MockS3()
    {
        svr.stop();
        thread.join();
    }

    S3StoreConfig config() const
    {
        S3StoreConfig c;
        c.endpoint = "http://127.0.0.1:" + std::to_string (port);
        c.bucket = "bkt";
        c.region = "fsn1";
        c.accessKey = "AKIAMOCK";
        c.secretKey = "the-mock-secret-key";
        c.backoffBaseMs = 5;
        return c;
    }

    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::mutex mutex;
    std::map<std::string, std::string> objects;
    std::map<std::string, std::map<int, std::string>> parts;
    std::string lastAuth, lastHost, lastPath, lastPayloadHash, completeBody, lastIfNoneMatch, lastIfMatch;
    int requests = 0, failNext = 0;
};
} // namespace

ROVOX_TEST (storage, s3_client_signs_requests_and_addresses_path_style)
{
    MockS3 s3;
    auto store = makeS3ObjectStore (s3.config());

    REQUIRE (store->put ("rovox/a/b.json", "{\"x\":1}", "application/json").ok());
    CHECK (s3.objects["rovox/a/b.json"] == "{\"x\":1}");
    CHECK (s3.lastPath == "/bkt/rovox/a/b.json");
    CHECK (s3.lastAuth.rfind ("AWS4-HMAC-SHA256 Credential=AKIAMOCK/", 0) == 0);
    CHECK (s3.lastAuth.find ("/fsn1/s3/aws4_request") != std::string::npos);
    CHECK (s3.lastAuth.find ("the-mock-secret-key") == std::string::npos);
    CHECK (s3.lastPayloadHash == sha256Hex ("{\"x\":1}"));
    CHECK (s3.lastHost == "127.0.0.1:" + std::to_string (s3.port));

    auto body = store->get ("rovox/a/b.json");
    REQUIRE (body.ok());
    CHECK (body.value() == "{\"x\":1}");

    auto meta = store->head ("rovox/a/b.json");
    REQUIRE (meta.ok());
    CHECK (meta.value().size == 7);

    auto missing = store->get ("nothing");
    REQUIRE (! missing.ok());
    CHECK (missing.error().code == errc::notFound);
    CHECK (! store->head ("nothing").ok());

    const auto dir = test::makeTempDir ("s3c");
    std::ofstream (dir / "up.bin", std::ios::binary) << "file body";
    REQUIRE (store->putFile ("rovox/up.bin", dir / "up.bin").ok());
    CHECK (s3.objects["rovox/up.bin"] == "file body");
    REQUIRE (store->getFile ("rovox/up.bin", dir / "down" / "up.bin").ok());
    CHECK (fs::file_size (dir / "down" / "up.bin") == 9);
    CHECK (! fs::exists (dir / "down" / "up.bin.part"));
    CHECK (! store->getFile ("rovox/none", dir / "down" / "none").ok());
    CHECK (! fs::exists (dir / "down" / "none"));

    REQUIRE (store->remove ("rovox/up.bin").ok());
    CHECK (s3.objects.count ("rovox/up.bin") == 0);
    CHECK (store->remove ("rovox/up.bin").ok());                // deleting what is gone is fine
}

ROVOX_TEST (storage, s3_client_lists_with_prefixes_and_pagination)
{
    MockS3 s3;
    auto store = makeS3ObjectStore (s3.config());

    for (const char* k : { "p/tenants/a/projects/p_1/HEAD", "p/tenants/b/projects/p_2/HEAD", "p/flat1", "p/flat2", "p/flat3", "other/x" })
        REQUIRE (store->put (k, "x").ok());

    auto folders = store->list ("p/tenants/", "/");
    REQUIRE (folders.ok());
    CHECK (folders.value().prefixes == std::vector<std::string> ({ "p/tenants/a/", "p/tenants/b/" }));
    CHECK (folders.value().objects.empty());

    auto flat = store->list ("p/flat");
    REQUIRE (flat.ok());
    CHECK (flat.value().objects.size() == 3);                   // three keys over two pages
    CHECK (flat.value().objects[0].key == "p/flat1" && flat.value().objects[0].size == 1);
}

ROVOX_TEST (storage, s3_client_retries_transient_errors_and_keeps_secrets_out_of_messages)
{
    MockS3 s3;
    auto cfg = s3.config();
    auto store = makeS3ObjectStore (cfg);

    s3.failNext = 2;
    REQUIRE (store->put ("k", "v").ok());                       // two 503s, then success
    CHECK (s3.requests == 3);

    s3.failNext = 100;
    auto r = store->put ("k2", "v");
    REQUIRE (! r.ok());
    CHECK (r.error().code == errc::busy);
    CHECK (r.error().message.find ("the-mock-secret-key") == std::string::npos);
    CHECK (r.error().message.find ("AKIAMOCK") == std::string::npos);
    s3.failNext = 0;

    cfg.endpoint = "http://127.0.0.1:1";                        // nothing listens there
    cfg.maxAttempts = 2;
    auto down = makeS3ObjectStore (cfg)->put ("k", "v");
    REQUIRE (! down.ok());
    CHECK (down.error().code == errc::busy);
}

ROVOX_TEST (storage, s3_client_uploads_large_files_in_parts)
{
    MockS3 s3;
    auto cfg = s3.config();
    cfg.multipartThreshold = 10;
    cfg.partSize = 10;
    auto store = makeS3ObjectStore (cfg);

    const auto dir = test::makeTempDir ("s3mp");
    const std::string content = "0123456789abcdefghijklmno";      // 25 bytes: three parts
    std::ofstream (dir / "big.wav", std::ios::binary) << content;
    REQUIRE (store->putFile ("big.wav", dir / "big.wav", "audio/wav").ok());
    CHECK (s3.objects["big.wav"] == content);
    CHECK (s3.parts["up1"].size() == 3);
    CHECK (s3.completeBody.find ("<PartNumber>3</PartNumber><ETag>\"part3\"</ETag>") != std::string::npos);

    std::ofstream (dir / "small.wav", std::ios::binary) << "tiny";
    REQUIRE (store->putFile ("small.wav", dir / "small.wav").ok());
    CHECK (s3.objects["small.wav"] == "tiny");                  // under the threshold: one PUT
}

ROVOX_TEST (storage, s3_client_sends_write_preconditions_and_maps_412)
{
    MockS3 s3;
    auto store = makeS3ObjectStore (s3.config());

    REQUIRE (store->putIf ("lease", "a", "application/json", { "*", "" }).ok());
    CHECK (s3.lastIfNoneMatch == "*");

    auto again = store->putIf ("lease", "b", "application/json", { "*", "" });          // the key exists now
    REQUIRE (! again.ok());
    CHECK (again.error().code == putConditionFailed);
    CHECK (s3.objects["lease"] == "a");

    auto meta = store->head ("lease");
    REQUIRE (meta.ok());
    REQUIRE (store->putIf ("lease", "c", "application/json", { "", meta.value().etag }).ok());
    CHECK (s3.lastIfMatch == "\"etag\"");
    CHECK (s3.objects["lease"] == "c");

    auto stale = store->putIf ("lease", "d", "application/json", { "", "\"older\"" });
    REQUIRE (! stale.ok());
    CHECK (stale.error().code == putConditionFailed);
    CHECK (s3.objects["lease"] == "c");
}

ROVOX_TEST (storage, s3_multipart_upload_stops_between_parts_when_cancelled)
{
    MockS3 s3;
    auto cfg = s3.config();
    cfg.multipartThreshold = 10;
    cfg.partSize = 10;
    auto store = makeS3ObjectStore (cfg);

    const auto dir = test::makeTempDir ("s3mpcancel");
    std::ofstream (dir / "big.wav", std::ios::binary) << "0123456789abcdefghijklmno";
    int polls = 0;
    auto r = store->putFileCancellable ("big.wav", dir / "big.wav", "audio/wav", [&] { return ++polls > 1; });
    REQUIRE (! r.ok());
    CHECK (r.error().code == errc::cancelled);
    CHECK (s3.parts["up1"].size() == 1);                        // stopped before part 2
    CHECK (s3.objects.count ("big.wav") == 0);                  // never completed
}

ROVOX_TEST (storage, s3_client_reports_the_s3_error_code_of_a_failed_download)
{
    MockS3 s3;
    auto store = makeS3ObjectStore (s3.config());
    const auto dir = test::makeTempDir ("s3err");
    auto r = store->getFile ("rovox/none", dir / "none.bin");
    REQUIRE (! r.ok());
    CHECK (r.error().code == errc::notFound);
    CHECK (r.error().message.find ("NoSuchKey") != std::string::npos);      // the code of the error document, not just the HTTP status
    CHECK (! fs::exists (dir / "none.bin"));
    CHECK (! fs::exists (dir / "none.bin.part"));
}

ROVOX_TEST (storage, s3_client_never_leaves_a_truncated_download_in_place)
{
    httplib::Server svr;
    svr.Get (R"(/bkt/(.+))", [] (const httplib::Request&, httplib::Response& res)
    {
        // announces 100 bytes, sends 10 and drops the connection
        res.set_content_provider (100, "application/octet-stream", [] (size_t, size_t, httplib::DataSink& sink)
        {
            sink.write ("0123456789", 10);
            return false;                                   // the provider fails: the connection drops short of the announced length
        });
    });
    const int port = svr.bind_to_any_port ("127.0.0.1");
    std::thread server ([&] { svr.listen_after_bind(); });

    for (int i = 0; i < 200 && ! svr.is_running(); ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

    S3StoreConfig c;
    c.endpoint = "http://127.0.0.1:" + std::to_string (port);
    c.bucket = "bkt";
    c.accessKey = "AKIAMOCK";
    c.secretKey = "the-mock-secret-key";
    c.backoffBaseMs = 5;
    c.maxAttempts = 2;
    auto store = makeS3ObjectStore (c);

    const auto dir = test::makeTempDir ("s3trunc");
    const auto dest = dir / "big.bin";
    std::ofstream (dest, std::ios::binary) << "previous good content";
    auto r = store->getFile ("big.bin", dest);
    CHECK (! r.ok());
    CHECK (fs::exists (dest) && fs::file_size (dest) == 21);                // the older file was not replaced by the partial one
    CHECK (! fs::exists (dir / "big.bin.part"));

    svr.stop();
    server.join();
}

ROVOX_TEST (storage, s3_client_raises_the_part_size_to_stay_within_10000_parts)
{
    MockS3 s3;
    auto cfg = s3.config();
    cfg.multipartThreshold = 10;
    cfg.partSize = 1;                                           // a 10 001 byte file would need 10 001 parts of this size
    auto store = makeS3ObjectStore (cfg);

    const auto dir = test::makeTempDir ("s3parts");
    const std::string content (10001, 'z');
    std::ofstream (dir / "huge.wav", std::ios::binary) << content;
    REQUIRE (store->putFile ("huge.wav", dir / "huge.wav", "audio/wav").ok());
    CHECK (s3.objects["huge.wav"] == content);
    CHECK (! s3.parts["up1"].empty() && s3.parts["up1"].size() <= 10000);
}

//==============================================================================
ROVOX_TEST (storage, project_survives_losing_the_working_cache)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "server-a");
    rs->start();

    std::string id;
    fs::path dir;
    {
        test::Env env;
        env.pm.setStorage (rs);
        id = env.newProject ("Cloud song");
        const auto wavDir = test::makeTempDir ("cloudwav");
        test::writeWav (wavDir / "take.wav", { test::sine (440, -12, 1.0, 44100) }, 44100);
        auto plan = env.ok (id, "import.plan", { { "paths", { (wavDir / "take.wav").string() } } });
        env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
        env.ok (id, "fx.add", { { "track_id", env.ok (id, "track.list")["tracks"][0]["track_id"] }, { "builtin", "compressor" } });
        env.ok (id, "project.save");
        dir = env.project (id).dir();

        REQUIRE (waitFor ([&] { return rs->status ("local", id)["synced_revision"] == env.project (id).revision(); }));
        const auto st = env.ok (id, "project.get")["storage"];
        CHECK (st["backend"] == "s3");
        CHECK (st["pending_uploads"] == 0);
        CHECK (st["synced_revision"] == env.project (id).revision());

        const auto list = env.ok ("", "project.list")["projects"];
        REQUIRE (list.size() >= 1);
        CHECK (list[0]["storage"]["backend"] == "s3");

        // the writer lease keeps a second server out
        auto other = makeStorage (store, "server-b");
        auto refused = other->acquireLease ("local", id);
        REQUIRE (! refused.ok());
        CHECK (refused.error().details.value ("lease_owner", "") == "server-a");

        REQUIRE (env.pm.close (id, SaveMode::save).ok());
    }

    REQUIRE (rs->flush (std::chrono::seconds (10)));
    const auto trackCount = [&] (const fs::path& d) { return summarizeEdit (d / editFileName).tracks; };
    const auto before = trackCount (dir);
    CHECK (before == 1);
    const auto mediaFiles = std::distance (fs::directory_iterator (dir / "media"), fs::directory_iterator());
    CHECK (mediaFiles == 1);

    // the cache is gone (a new container with an empty volume)
    std::error_code ec;
    fs::remove_all (dir.parent_path(), ec);
    REQUIRE (! fs::exists (dir));

    {
        test::Env env;
        env.pm.setStorage (rs);
        auto catalog = env.ok ("", "project.catalog")["projects"];
        const auto it = std::find_if (catalog.begin(), catalog.end(), [&] (const json& p) { return p["project_id"] == id; });
        REQUIRE (it != catalog.end());
        CHECK ((*it)["remote_only"] == true);
        CHECK ((*it)["tracks"] == 1);
        CHECK ((*it)["name"] == "Cloud song");

        auto opened = env.pm.open (id);
        REQUIRE (opened.ok());
        auto& p = env.project (id);
        CHECK (te::getAudioTracks (p.edit()).size() == 1);
        CHECK (p.name() == "Cloud song");
        CHECK (std::distance (fs::directory_iterator (p.dir() / "media"), fs::directory_iterator()) == 1);
        CHECK (trackCount (p.dir()) == before);
        env.pm.close (id, SaveMode::discard);
    }

    rs->shutdown (std::chrono::seconds (5));
}

ROVOX_TEST (storage, open_by_name_and_cache_refresh_from_a_newer_head)
{
    auto store = std::make_shared<FakeStore>();
    auto rs = makeStorage (store, "server-a");
    rs->start();
    test::Env env;
    env.pm.setStorage (rs);
    const auto id = env.newProject ("Named in bucket");
    env.ok (id, "project.update", { { "name", "Named in bucket" } });
    env.ok (id, "project.save");
    const auto dir = env.project (id).dir();
    REQUIRE (waitFor ([&] { return rs->status ("local", id)["synced_revision"] == env.project (id).revision(); }));
    REQUIRE (env.pm.close (id, SaveMode::save).ok());
    REQUIRE (rs->flush (std::chrono::seconds (10)));

    // somebody else saved revision +5 meanwhile: the cache must follow HEAD on open
    const auto current = rs->fetchHead ("local", id).value();
    const auto newer = current.revision + 5;
    auto manifest = json::parse (store->objects[rs->keys().revFile ("local", id, current.revision, "rovox.json")]);
    manifest["revision"] = newer;
    manifest["name"] = "Renamed elsewhere";
    store->objects[rs->keys().revFile ("local", id, newer, "rovox.json")] = manifest.dump();
    auto editXml = store->objects[rs->keys().revFile ("local", id, current.revision, "edit.xml")];
    const std::string marker = "rovox:revision=\"";
    const auto at = editXml.find (marker);
    REQUIRE (at != std::string::npos);
    editXml.replace (at + marker.size(), editXml.find ('"', at + marker.size()) - (at + marker.size()), std::to_string (newer));      // edit.xml pins the revision
    store->objects[rs->keys().revFile ("local", id, newer, "edit.xml")] = editXml;
    auto head = current;
    head.revision = newer;
    head.name = "Renamed elsewhere";
    store->objects[rs->keys().head ("local", id)] = head.toJson().dump();

    auto opened = env.pm.open (id);
    REQUIRE (opened.ok());
    CHECK (env.project (id).revision() == newer);
    CHECK (env.project (id).manifest().name == "Renamed elsewhere");
    env.pm.close (id, SaveMode::discard);

    // a project that exists only in the bucket can be opened by its name
    std::error_code ec;
    fs::remove_all (dir.parent_path(), ec);
    auto byName = env.pm.open ("Renamed elsewhere");
    REQUIRE (byName.ok());
    CHECK (byName.value() == id);
    env.pm.close (id, SaveMode::discard);
    rs->shutdown (std::chrono::seconds (5));
}

ROVOX_TEST (storage, a_second_server_cannot_open_a_project_for_writing)
{
    auto store = std::make_shared<FakeStore>();
    auto a = makeStorage (store, "server-a"), b = makeStorage (store, "server-b");
    a->start();
    b->start();
    test::Env first, second;
    first.pm.setStorage (a);
    second.pm.setStorage (b);

    const auto id = first.newProject ("Shared");
    first.ok (id, "project.save");
    REQUIRE (waitFor ([&] { return a->status ("local", id)["synced_revision"] == first.project (id).revision(); }));
    b->refreshHeads();

    auto refused = second.pm.open (id);
    REQUIRE (! refused.ok());
    CHECK (refused.error().code == errc::busy);
    CHECK (refused.error().message.find ("server-a") != std::string::npos);

    auto readOnly = second.pm.open (id, { .readOnly = true });      // reading never needs the lease (the local flock is the same directory here, so allow busy)
    CHECK (readOnly.ok() || readOnly.error().code == errc::busy);

    first.pm.close (id, SaveMode::save);
    REQUIRE (a->flush (std::chrono::seconds (10)));
    a->shutdown (std::chrono::seconds (5));
    b->shutdown (std::chrono::seconds (5));
}

//==============================================================================
// FsObjectStore: the same RemoteStorage scenarios over a directory (a store factory per backend), plus what is specific to files.
namespace
{
using StoreFactory = std::function<std::shared_ptr<ObjectStore>()>;

std::string readFile (const fs::path& f)
{
    std::ifstream in (f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::pair<std::string, StoreFactory>> backends (const std::string& tag)
{
    auto fake = std::make_shared<FakeStore>();
    const auto dir = test::makeTempDir (tag);
    return { { "fake", [fake] { return std::static_pointer_cast<ObjectStore> (fake); } },
             { "fs", [dir] { return makeFsObjectStore (dir); } } };      // every call is another "process" on the same root
}

void leaseScenario (const StoreFactory& make, const std::string& id, bool conditional)
{
    auto store = make();
    std::int64_t now = 1000;
    const ProjectLease::Clock clock = [&] { return now; };
    const std::string key = "rovox/tenants/local/projects/" + id + "/LEASE";

    REQUIRE (ProjectLease::acquire (*store, key, "server-a", 60, clock, conditional).ok());
    CHECK (ProjectLease::acquire (*store, key, "server-a", 60, clock, conditional).ok());
    auto other = ProjectLease::acquire (*make(), key, "server-b", 60, clock, conditional);
    REQUIRE (! other.ok());
    CHECK (other.error().code == errc::busy);
    CHECK (other.error().details.value ("lease_owner", "") == "server-a");

    now = 1030;
    CHECK (ProjectLease::renew (*store, key, "server-a", 60, clock, conditional).ok());
    CHECK (ProjectLease::read (*store, key).value()->expires == 1090);

    now = 1100;                                                         // expired: take-over
    CHECK (ProjectLease::acquire (*make(), key, "server-b", 60, clock, conditional).ok());
    auto lost = ProjectLease::renew (*store, key, "server-a", 60, clock, conditional);
    REQUIRE (! lost.ok());
    CHECK (lost.error().details.contains ("lease_owner"));

    ProjectLease::release (*store, key, "server-a");                    // not the owner: no effect
    CHECK (ProjectLease::read (*store, key).value().has_value());
    ProjectLease::release (*store, key, "server-b");
    CHECK (! ProjectLease::read (*store, key).value().has_value());
}

void conditionalScenario (const StoreFactory& make, const std::string& id)
{
    auto store = make();
    const auto key = "rovox/" + id + "/lease";

    REQUIRE (store->putIf (key, "a", "application/json", { "*", "" }).ok());
    auto again = make()->putIf (key, "b", "application/json", { "*", "" });
    REQUIRE (! again.ok());
    CHECK (again.error().code == putConditionFailed);
    CHECK (store->get (key).value() == "a");

    auto meta = store->head (key);
    REQUIRE (meta.ok());
    CHECK (meta.value().size == 1);
    REQUIRE (store->putIf (key, "c", "application/json", { "", meta.value().etag }).ok());

    auto stale = store->putIf (key, "d", "application/json", { "", meta.value().etag });      // the ETag moved on
    REQUIRE (! stale.ok());
    CHECK (stale.error().code == putConditionFailed);
    CHECK (store->get (key).value() == "c");

    auto missing = store->putIf (key + "-none", "x", "application/json", { "", "\"nothing\"" });
    REQUIRE (! missing.ok());
    CHECK (missing.error().code == putConditionFailed);
    CHECK (! store->head (key + "-none").ok());
}

void pushPullScenario (const StoreFactory& make)
{
    auto rs = std::make_shared<RemoteStorage> (make(), StorageKeys { "rovox/" }, "test", "server-a", 60);
    auto c = makeCache ("local", 4);
    REQUIRE (rs->push (jobFor (c)).ok());
    REQUIRE (rs->push (jobFor (c)).ok());                               // same revision: idempotent
    auto head = rs->fetchHead ("local", c.id);
    REQUIRE (head.ok());
    CHECK (head.value().revision == 4);

    auto fresh = std::make_shared<RemoteStorage> (make(), StorageKeys { "rovox/" }, "test", "server-b", 60);
    const auto target = test::makeTempDir ("fspull") / "project.rvx";
    auto rev = fresh->pull ("local", c.id, target);
    REQUIRE (rev.ok());
    CHECK (rev.value() == 4);
    CHECK (readFile (target / "edit.xml") == readFile (c.dir / "edit.xml"));
    CHECK (readFile (target / "media" / (shaA + ".wav")) == "RIFFfakewave");

    REQUIRE (rs->acquireLease ("local", c.id).ok());
    auto refused = fresh->acquireLease ("local", c.id);
    REQUIRE (! refused.ok());
    CHECK (refused.error().code == errc::busy);
}
} // namespace

ROVOX_TEST (storage, fs_store_runs_the_remote_storage_scenarios)
{
    for (auto& [name, make] : backends ("fsstore"))
    {
        leaseScenario (make, "p_lease_a_" + name, false);
        leaseScenario (make, "p_lease_b_" + name, true);
        conditionalScenario (make, "cond_" + name);
        pushPullScenario (make);
    }
}

ROVOX_TEST (storage, fs_store_two_concurrent_acquirers_exactly_one_wins)
{
    const auto dir = test::makeTempDir ("fsrace");

    for (int round = 0; round < 8; ++round)
    {
        // two servers = two store objects on one root; the lease object is created by whoever takes the directory lock first
        auto b = std::make_shared<RemoteStorage> (makeFsObjectStore (dir), StorageKeys { "rovox/" }, "fs", "server-b", 60);
        auto c = std::make_shared<RemoteStorage> (makeFsObjectStore (dir), StorageKeys { "rovox/" }, "fs", "server-c", 60);
        REQUIRE (b->conditionalWritesActive());
        REQUIRE (c->conditionalWritesActive());

        const auto project = "p_fsrace_" + std::to_string (round);
        std::atomic<bool> go { false };
        std::atomic<int> winners { 0 }, busy { 0 };
        const auto race = [&] (const std::shared_ptr<RemoteStorage>& rs)
        {
            while (! go)
                std::this_thread::yield();

            auto r = rs->acquireLease ("local", project);

            if (r.ok())
                ++winners;
            else if (r.error().code == errc::busy && r.error().details.contains ("lease_owner"))
                ++busy;
        };
        std::thread t1 ([&] { race (b); }), t2 ([&] { race (c); });
        go = true;
        t1.join();
        t2.join();
        CHECK (winners == 1);
        CHECK (busy == 1);
    }

    // eight raw If-None-Match puts of one key: one winner
    std::atomic<bool> go { false };
    std::atomic<int> won { 0 }, lost { 0 };
    std::vector<std::thread> threads;

    for (int i = 0; i < 8; ++i)
        threads.emplace_back ([&, i]
        {
            auto store = makeFsObjectStore (dir);

            while (! go)
                std::this_thread::yield();

            auto r = store->putIf ("raw/key", "writer-" + std::to_string (i), "text/plain", { "*", "" });

            if (r.ok())
                ++won;
            else if (r.error().code == putConditionFailed)
                ++lost;
        });

    go = true;

    for (auto& t : threads)
        t.join();

    CHECK (won == 1);
    CHECK (lost == 7);
}

ROVOX_TEST (storage, fs_store_keys_etag_list_and_files)
{
    const auto dir = test::makeTempDir ("fskeys");
    auto store = makeFsObjectStore (dir / "root");

    for (const char* bad : { "../escape", "a/../../escape", "/abs/key", "a//b", "a/", "", "./x", "a/./b", "..", ".rovox-lock", "a/.rovox-tmp-1", "k.etag" })
    {
        const auto put = store->put (bad, "x");
        REQUIRE (! put.ok());
        CHECK (put.error().code == errc::invalidParams);
        CHECK (store->get (bad).error().code == errc::invalidParams);
        CHECK (store->head (bad).error().code == errc::invalidParams);
        CHECK (store->remove (bad).error().code == errc::invalidParams);
    }

    CHECK (! fs::exists (dir / "escape"));
    CHECK (store->list ("../").error().code == errc::invalidParams);

    CHECK (store->head ("missing").error().code == errc::notFound);
    CHECK (store->get ("missing").error().code == errc::notFound);

    // ETag = quoted sha256 of the content
    REQUIRE (store->put ("b/two", "hello").ok());
    CHECK (store->head ("b/two").value().etag == "\"" + sha256Hex ("hello") + "\"");
    REQUIRE (store->put ("b/two", "hello world").ok());
    CHECK (store->head ("b/two").value().etag == "\"" + sha256Hex ("hello world") + "\"");

    // a sidecar that no longer matches the file (a crash between the two renames) is ignored
    std::ofstream (dir / "root" / "b" / "two", std::ios::binary | std::ios::trunc) << "changed behind the back!";
    CHECK (store->head ("b/two").value().etag == "\"" + sha256Hex ("changed behind the back!") + "\"");

    REQUIRE (store->put ("a/one", "1").ok());
    REQUIRE (store->put ("b/one", "22").ok());
    REQUIRE (store->put ("a/sub/z", "3").ok());
    REQUIRE (store->put ("a/sub/a", "4").ok());
    REQUIRE (store->put ("top", "5").ok());

    auto all = store->list ("");
    REQUIRE (all.ok());
    std::vector<std::string> keys;

    for (auto& o : all.value().objects)
        keys.push_back (o.key);

    CHECK ((keys == std::vector<std::string> { "a/one", "a/sub/a", "a/sub/z", "b/one", "b/two", "top" }));   // lexicographic like S3, no lock / sidecar / temp files
    CHECK (all.value().prefixes.empty());

    auto under = store->list ("a/");
    REQUIRE (under.ok());
    CHECK (under.value().objects.size() == 3);

    auto folded = store->list ("a/", "/");
    REQUIRE (folded.ok());
    REQUIRE (folded.value().objects.size() == 1);
    CHECK (folded.value().objects[0].key == "a/one");
    CHECK ((folded.value().prefixes == std::vector<std::string> { "a/sub/" }));

    auto partial = store->list ("b/o");
    REQUIRE (partial.ok());
    CHECK (partial.value().objects.size() == 1);
    CHECK (partial.value().objects[0].size == 2);

    // files in and out
    const auto src = dir / "source.bin";
    std::ofstream (src, std::ios::binary) << std::string (3 * 1024 * 1024 + 5, 'q');
    REQUIRE (store->putFile ("big/blob", src).ok());
    CHECK (store->head ("big/blob").value().size == 3 * 1024 * 1024 + 5);
    CHECK (store->head ("big/blob").value().etag == "\"" + *sha256File (src) + "\"");
    REQUIRE (store->getFile ("big/blob", dir / "out" / "copy.bin").ok());
    CHECK (readFile (dir / "out" / "copy.bin") == readFile (src));
    CHECK (! fs::exists (dir / "out" / "copy.bin.part"));

    REQUIRE (store->remove ("b/two").ok());
    CHECK (store->head ("b/two").error().code == errc::notFound);
    CHECK (! fs::exists (dir / "root" / "b" / "two.etag"));
    CHECK (store->remove ("b/two").ok());                               // removing a missing key is not an error
}

ROVOX_TEST (storage, fs_store_put_interrupted_before_the_rename_leaves_nothing_visible)
{
    const auto dir = test::makeTempDir ("fscrash");
    FsObjectStore store (dir);

    store.setBeforeCommitHook ([] { return true; });                    // "crash" with the temporary file complete
    CHECK (! store.put ("new/object", "payload").ok());
    CHECK (store.head ("new/object").error().code == errc::notFound);
    CHECK (store.list ("").value().objects.empty());

    store.setBeforeCommitHook (nullptr);
    REQUIRE (store.put ("new/object", "old").ok());
    store.setBeforeCommitHook ([] { return true; });
    CHECK (! store.put ("new/object", "newer").ok());
    CHECK (store.get ("new/object").value() == "old");                  // the previous version is intact
    CHECK (store.head ("new/object").value().etag == "\"" + sha256Hex ("old") + "\"");

    // a stray temporary file (the process died) is neither listed nor readable as an object
    std::ofstream (dir / "new" / ".rovox-tmp-999-1-1") << "half";
    CHECK (store.list ("").value().objects.size() == 1);
    CHECK (store.head (".rovox-tmp-999-1-1").error().code == errc::invalidParams);

    // a failed source read leaves nothing either
    store.setBeforeCommitHook (nullptr);
    CHECK (! store.putFile ("from/missing", dir / "no-such-file").ok());
    CHECK (store.head ("from/missing").error().code == errc::notFound);
    CHECK (store.list ("from/").value().objects.empty());
}
