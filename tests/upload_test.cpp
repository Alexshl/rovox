// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "HttpServer.h"
#include "Sha1.h"
#include "Sha256.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <httplib.h>

#include <atomic>
#include <fstream>
#include <thread>

using namespace rovox;

namespace
{
std::string patternData (size_t n, unsigned seed)
{
    std::string s (n, '\0');
    std::uint32_t x = seed * 2654435761u + 1;

    for (auto& c : s)
    {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        c = (char) (x & 0xff);
    }

    return s;
}

std::string b64sha1 (const std::string& chunk)
{
    Sha1 h;
    h.update (chunk.data(), chunk.size());
    const auto d = h.digest();
    return base64Encode (d.data(), d.size());
}

/** The message loop must run for command routes; these tests only use upload/health routes, which never touch it. */
struct Fixture
{
    Fixture (ServerOptions so = {})
    {
        so.port = 0;
        server = std::make_unique<HttpServer> (test::host(), env.pm, env.bus, env.hub, so);
        REQUIRE (server->start().ok());
        client = std::make_unique<httplib::Client> ("127.0.0.1", server->port());
    }

    test::Env env;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<httplib::Client> client;
};

std::string tusCreate (httplib::Client& c, size_t length, const std::string& filename, const std::string& relpath = "", const std::string& batch = "", const httplib::Headers& extra = {})
{
    auto b64 = [] (const std::string& s) { return base64Encode (reinterpret_cast<const std::uint8_t*> (s.data()), s.size()); };
    httplib::Headers h = extra;
    h.emplace ("Tus-Resumable", "1.0.0");
    h.emplace ("Upload-Length", std::to_string (length));
    h.emplace ("Upload-Metadata", "filename " + b64 (filename) + ",relpath " + b64 (relpath) + ",batch_id " + b64 (batch));
    auto r = c.Post ("/api/uploads", h, "", "");
    REQUIRE (r != nullptr);
    REQUIRE (r->status == 201);
    return r->get_header_value ("Location");
}
} // namespace

ROVOX_TEST (upload, healthz_and_options)
{
    Fixture f;
    auto h = f.client->Get ("/healthz");
    REQUIRE (h != nullptr);
    CHECK (h->status == 200);
    CHECK (json::parse (h->body)["status"] == "ok");

    auto o = f.client->Options ("/api/uploads");
    REQUIRE (o != nullptr);
    CHECK (o->status == 204);
    CHECK (o->get_header_value ("Tus-Resumable") == "1.0.0");
    CHECK (o->get_header_value ("Tus-Extension").find ("creation") != std::string::npos);
    CHECK (o->get_header_value ("Tus-Extension").find ("checksum") != std::string::npos);
}

ROVOX_TEST (upload, token_is_required_when_configured)
{
    ServerOptions so;
    so.token = "s3cret-token";
    Fixture f (so);
    CHECK (f.client->Get ("/healthz")->status == 200);
    CHECK (f.client->Get ("/api/projects")->status == 401);
    auto denied = f.client->Post ("/api/uploads", httplib::Headers { { "Upload-Length", "10" } }, "", "");
    CHECK (denied->status == 401);
    CHECK (denied->body.find ("s3cret-token") == std::string::npos);        // secrets never echo back

    httplib::Headers auth { { "Authorization", "Bearer s3cret-token" }, { "Tus-Resumable", "1.0.0" }, { "Upload-Length", "10" } };
    CHECK (f.client->Post ("/api/uploads", auth, "", "")->status == 201);
}

ROVOX_TEST (upload, resume_after_a_dropped_connection_ends_with_identical_sha256)
{
    Fixture f;
    const auto data = patternData (3 * 1024 * 1024 + 123, 7);
    const auto loc = tusCreate (*f.client, data.size(), "REC01_01.WAV");
    const auto id = loc.substr (loc.rfind ('/') + 1);

    auto patch = [&] (httplib::Client& c, size_t offset, size_t length, const std::string& checksum = "")
    {
        httplib::Headers h { { "Tus-Resumable", "1.0.0" }, { "Upload-Offset", std::to_string (offset) } };

        if (! checksum.empty())
            h.emplace ("Upload-Checksum", checksum);

        return c.Patch (loc, h, data.substr (offset, length), "application/offset+octet-stream");
    };

    // first client sends the first chunk and is "killed"
    auto first = patch (*f.client, 0, 1 << 20, "sha1 " + b64sha1 (data.substr (0, 1 << 20)));
    REQUIRE (first != nullptr);
    CHECK (first->status == 204);
    CHECK (first->get_header_value ("Upload-Offset") == std::to_string (1 << 20));

    // a new client asks where the server is, and continues from there
    httplib::Client second ("127.0.0.1", f.server->port());
    auto head = second.Get (loc);                        // HEAD is served by the GET route: same headers
    REQUIRE (head != nullptr);
    CHECK (head->get_header_value ("Upload-Offset") == std::to_string (1 << 20));

    // wrong offset is refused and reports the real one
    auto stale = patch (second, 0, 100);
    CHECK (stale->status == 409);
    CHECK (stale->get_header_value ("Upload-Offset") == std::to_string (1 << 20));

    // bad chunk checksum: refused with 460 and the chunk is discarded
    auto corrupt = patch (second, 1 << 20, 1 << 20, "sha1 " + b64sha1 ("something else"));
    CHECK (corrupt->status == 460);
    CHECK (second.Get (loc)->get_header_value ("Upload-Offset") == std::to_string (1 << 20));

    size_t offset = 1 << 20;

    while (offset < data.size())
    {
        const auto n = std::min<size_t> (1 << 20, data.size() - offset);
        auto r = patch (second, offset, n, "sha256 " + [&]
        {
            Sha256 h;
            h.update (data.data() + offset, n);
            const auto d = h.digest();
            return base64Encode (d.data(), d.size());
        }());
        REQUIRE (r != nullptr);
        REQUIRE (r->status == 204);
        offset += n;
    }

    const auto done = f.server->uploads().completedPath (id);
    REQUIRE (fs::exists (done));
    CHECK (sha256File (done) == sha256Hex (data));
    CHECK (fs::file_size (done) == data.size());

    // patching a finished upload is refused; the part file is gone
    CHECK (patch (second, 0, 10)->status == 409);
    CHECK (! fs::exists (f.server->uploads().partPath (id)));

    // termination
    const auto loc2 = tusCreate (*f.client, 10, "x");
    CHECK (f.client->Delete (loc2, httplib::Headers { { "Tus-Resumable", "1.0.0" } })->status == 204);
    CHECK (f.client->Get (loc2)->status == 404);
}

ROVOX_TEST (upload, limits_free_space_full_hash_and_unguessable_ids)
{
    ServerOptions so;
    so.uploadLimits.maxFileBytes = 1000;
    Fixture f (so);
    httplib::Headers h { { "Tus-Resumable", "1.0.0" }, { "Upload-Length", "1001" } };
    CHECK (f.client->Post ("/api/uploads", h, "", "")->status == 400);

    // announced hash that does not match the content fails the last chunk
    const std::string data = patternData (500, 1);
    auto b64 = [] (const std::string& s) { return base64Encode (reinterpret_cast<const std::uint8_t*> (s.data()), s.size()); };
    httplib::Headers create { { "Tus-Resumable", "1.0.0" }, { "Upload-Length", "500" }, { "Upload-Metadata", "sha256 " + b64 (std::string (64, 'a')) } };
    const auto loc = f.client->Post ("/api/uploads", create, "", "")->get_header_value ("Location");
    auto r = f.client->Patch (loc, httplib::Headers { { "Upload-Offset", "0" } }, data, "application/offset+octet-stream");
    CHECK (r->status == 400);

    // ids carry 128 random bits
    std::set<std::string> ids;

    for (int i = 0; i < 20; ++i)
        ids.insert (f.client->Post ("/api/uploads", h = { { "Tus-Resumable", "1.0.0" }, { "Upload-Length", "10" } }, "", "")->get_header_value ("Location"));

    CHECK (ids.size() == 20);
    CHECK ((*ids.begin()).size() == std::string ("/api/uploads/up_").size() + 32);

    // unknown / malformed ids never touch the file system
    CHECK (f.client->Get ("/api/uploads/..%2f..%2fetc")->status == 404);
    CHECK (f.client->Get ("/api/uploads/up_zzzz")->status == 404);
}

ROVOX_TEST (upload, batch_of_a_zprj_folder_reports_progress_and_completion)
{
    Fixture f;
    auto sub = f.env.hub.subscribe ({});
    const std::vector<std::pair<std::string, std::string>> files = { { "ZOOM0001.zprj/REC01_01.WAV", patternData (2000, 1) },
                                                                    { "ZOOM0001.zprj/REC02_01.WAV", patternData (3000, 2) } };
    json request { { "files", json::array() } };

    for (auto& [rel, data] : files)
        request["files"].push_back ({ { "relpath", rel }, { "size", data.size() }, { "sha256", sha256Hex (data) } });

    auto created = f.client->Post ("/api/upload-batches", request.dump(), "application/json");
    REQUIRE (created != nullptr);
    REQUIRE (created->status == 201);
    const auto batch = json::parse (created->body);
    CHECK (batch["files"].size() == 2);

    CHECK (f.client->Post ("/api/upload-batches", json { { "files", json::array ({ { { "relpath", "../evil" }, { "size", 1 } } }) } }.dump(), "application/json")->status == 400);

    auto status = json::parse (f.client->Get ("/api/upload-batches/" + batch["batch_id"].get<std::string>())->body);
    CHECK (status["complete"] == false);
    CHECK (status["total_bytes"] == 5000);

    for (size_t i = 0; i < files.size(); ++i)
    {
        auto r = f.client->Patch (batch["files"][i]["url"].get<std::string>(), httplib::Headers { { "Upload-Offset", "0" } }, files[i].second, "application/offset+octet-stream");
        REQUIRE (r->status == 204);
    }

    status = json::parse (f.client->Get ("/api/upload-batches/" + batch["batch_id"].get<std::string>())->body);
    CHECK (status["complete"] == true);
    CHECK (status["received_bytes"] == 5000);

    int completed = 0, batchCompleted = 0;
    Event e;

    while (sub->tryPop (e))
    {
        completed += e.body["type"] == "upload.completed" ? 1 : 0;
        batchCompleted += e.body["type"] == "upload.batch_completed" ? 1 : 0;
    }

    CHECK (completed == 2);
    CHECK (batchCompleted == 1);
}

ROVOX_TEST (upload, memory_does_not_grow_with_a_large_streamed_upload)
{
    Fixture f;
    const size_t total = 256u << 20;            // 256 MB through one PATCH
    const auto loc = tusCreate (*f.client, total, "big.wav");

    auto rssKb = [] { std::ifstream s ("/proc/self/status"); std::string l; while (std::getline (s, l)) if (l.rfind ("VmRSS", 0) == 0) return std::stol (l.substr (6)); return 0L; };
    const auto before = rssKb();
    long peak = before;
    std::atomic<bool> done { false };
    std::thread sampler ([&] { while (! done) { peak = std::max (peak, rssKb()); std::this_thread::sleep_for (std::chrono::milliseconds (20)); } });

    httplib::Headers h { { "Tus-Resumable", "1.0.0" }, { "Upload-Offset", "0" } };
    size_t sent = 0;
    auto r = f.client->Patch (loc, h, total, [&] (size_t offset, size_t length, httplib::DataSink& sink)
    {
        static const std::string block (1 << 20, 'x');
        sink.write (block.data(), std::min (length, block.size()));
        sent = offset + std::min (length, block.size());
        return true;
    }, "application/offset+octet-stream");
    done = true;
    sampler.join();

    REQUIRE (r != nullptr);
    CHECK (r->status == 204);
    CHECK (sent == total);
    std::cerr << "    RSS before " << before / 1024 << " MB, peak " << peak / 1024 << " MB for a " << total / (1 << 20) << " MB upload\n";
    CHECK (peak - before < 100 * 1024);         // < 100 MB over the baseline

    std::error_code ec;
    fs::remove (f.server->uploads().completedPath (loc.substr (loc.rfind ('/') + 1)), ec);
}

ROVOX_TEST (upload, cleanup_removes_stale_unfinished_uploads_only)
{
    UploadLimits limits;
    limits.ttl = std::chrono::seconds (60);
    UploadStore store (test::makeTempDir ("store"), limits);
    auto stale = store.create (10, { { "filename", "a" } });
    auto done = store.create (4, { { "filename", "b" } });
    REQUIRE (stale.ok());
    REQUIRE (done.ok());
    REQUIRE (store.append (done.value().id, 0, "abcd", 4).ok());
    CHECK (store.cleanup (std::time (nullptr)) == 0);
    CHECK (store.cleanup (std::time (nullptr) + 3600) == 1);
    CHECK (! store.info (stale.value().id).ok());
    CHECK (store.info (done.value().id).ok());
}

ROVOX_TEST (upload, sha1_and_base64_known_vectors)
{
    Sha1 h;
    h.update ("abc", 3);
    const auto d = h.digest();
    CHECK (base64Encode (d.data(), d.size()) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
    CHECK (base64Decode (base64Encode (reinterpret_cast<const std::uint8_t*> ("hello!"), 6)) == "hello!");
    CHECK (base64Decode ("***").empty());
}

ROVOX_TEST (upload, command_layer_over_http_with_a_running_message_loop)
{
    Fixture f;
    std::atomic<bool> finished { false };
    std::string projectId;
    int newStatus = 0, trackStatus = 0, infoStatus = 0, conflictStatus = 0, listCount = -1;
    std::uint64_t revision = 0;

    std::thread client ([&]
    {
        httplib::Client c ("127.0.0.1", f.server->port());
        auto created = c.Post ("/api/projects", json { { "name", "over http" } }.dump(), "application/json");
        newStatus = created ? created->status : -1;

        if (created && created->status == 200)
        {
            projectId = json::parse (created->body)["data"]["project_id"];
            auto t = c.Post ("/api/projects/" + projectId + "/commands", json { { "name", "track.create" }, { "params", { { "name", "A" } } }, { "origin", { { "kind", "ui" }, { "label", "alex" } } } }.dump(), "application/json");
            trackStatus = t ? t->status : -1;
            revision = json::parse (t->body)["revision"];
            auto info = c.Get ("/api/projects/" + projectId);
            infoStatus = info ? info->status : -1;
            auto stale = c.Post ("/api/projects/" + projectId + "/commands", json { { "name", "track.create" }, { "if_revision", 0 } }.dump(), "application/json");
            conflictStatus = stale ? stale->status : -1;
            listCount = (int) json::parse (c.Get ("/api/projects")->body)["data"]["projects"].size();
        }

        finished = true;
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);

    while (! finished && std::chrono::steady_clock::now() < deadline)
        test::pumpMessages (10);

    client.join();
    CHECK (newStatus == 200);
    CHECK (trackStatus == 200);
    CHECK (infoStatus == 200);
    CHECK (conflictStatus == 409);
    CHECK (listCount == 1);
    CHECK (revision >= 1);
    CHECK (f.env.run (projectId, "history.list").data["undo"][0]["origin"]["kind"] == "ui");
}
