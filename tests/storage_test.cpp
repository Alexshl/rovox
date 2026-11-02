// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Object storage as the source of truth: key layout, HEAD, the writer lease, push order (media, then snapshot, then HEAD), pull into an empty cache,
// background upload with retries, the S3 client against a local mock server (SigV4, path-style, retries, list, multipart) and a project that survives
// the loss of its cache. All against an in-memory fake store or a local HTTP server: no real network.
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
        std::lock_guard<std::mutex> lock (mutex);
        auto it = objects.find (key);

        if (it == objects.end())
            return makeError (errc::notFound, "no " + key);

        return ObjectMeta { key, "etag", it->second.size() };
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
        std::lock_guard<std::mutex> lock (mutex);

        if (failPuts > 0)
        {
            --failPuts;
            return makeError (errc::busy, "injected failure");
        }

        objects[key] = body;
        putLog.push_back (key);
        return {};
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
                out.objects.push_back ({ k, "etag", v.size() });
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
    std::vector<std::string> putLog;
    int failPuts = 0;
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

std::shared_ptr<RemoteStorage> makeStorage (const std::shared_ptr<FakeStore>& store, const std::string& owner, const std::string& prefix = "rovox/")
{
    return std::make_shared<RemoteStorage> (store, StorageKeys { prefix }, "fake", owner, 60);
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
    std::string lastAuth, lastHost, lastPath, lastPayloadHash, completeBody;
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
