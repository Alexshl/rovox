// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "HttpServer.h"
#include "Sha1.h"
#include "Sha256.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <httplib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <set>
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

        // the engine host (and so the upload directory) is shared by all tests: start each one from an empty store, the batch id is fixed
        std::error_code ec;
        const auto root = server->uploads().materialisedDir ("x").parent_path();

        for (auto& e : fs::directory_iterator (root, ec))
            fs::remove_all (e.path(), ec);

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

ROVOX_TEST (upload, cleanup_removes_stale_unfinished_uploads_and_finished_ones_after_the_ttl)
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
    CHECK (store.info (done.value().id).ok());
    CHECK (store.cleanup (std::time (nullptr) + 3600) == 2);
    CHECK (! store.info (stale.value().id).ok());
    CHECK (! store.info (done.value().id).ok());
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
    // REST is an agent over HTTP: the route decides the origin, the body's claim of being the UI is ignored
    CHECK (f.env.run (projectId, "history.list").data["undo"][0]["origin"]["kind"] == "mcp");
}

//==============================================================================
// E10 hardening: origin/host guard, content type, body limits, auth counting, Upload-Length, hashing outside the lock

ROVOX_TEST (upload, foreign_origin_foreign_host_and_non_json_posts_are_refused_without_a_token)
{
    Fixture f;
    const std::string body = R"({"name":"x"})";

    CHECK (f.client->Post ("/api/projects", httplib::Headers { { "Origin", "http://evil.example" } }, body, "application/json")->status == 403);
    CHECK (f.client->Get ("/api/projects", httplib::Headers { { "Origin", "http://evil.example" } })->status == 403);

    // DNS rebinding: the page's own origin equals the Host, but the server is bound to loopback
    CHECK (f.client->Get ("/api/projects", httplib::Headers { { "Host", "rebind.example" }, { "Origin", "http://rebind.example" } })->status == 403);
    CHECK (f.client->Get ("/api/projects", httplib::Headers { { "Host", "rebind.example" } })->status == 403);

    // a "simple" cross-origin POST never reaches the JSON handlers
    CHECK (f.client->Post ("/api/projects", body, "text/plain")->status == 415);
    CHECK (f.client->Post ("/api/projects", body, "application/x-www-form-urlencoded")->status == 415);

    // tus routes keep their own types
    httplib::Headers h { { "Tus-Resumable", "1.0.0" }, { "Upload-Length", "10" } };
    CHECK (f.client->Post ("/api/uploads", h, "", "")->status == 201);
}

ROVOX_TEST (upload, request_bodies_are_capped_except_on_upload_routes)
{
    ServerOptions so;
    so.maxBodyBytes = 1024;
    Fixture f (so);

    auto big = f.client->Post ("/api/login", std::string (4096, 'a'), "application/json");
    REQUIRE (big != nullptr);
    CHECK (big->status == 413);

    // uploads stream without the global cap
    const auto data = patternData (4096, 3);
    const auto loc = tusCreate (*f.client, data.size(), "ok.bin");
    CHECK (f.client->Patch (loc, httplib::Headers { { "Upload-Offset", "0" } }, data, "application/offset+octet-stream")->status == 204);
}

namespace
{
/** Sends a raw HTTP request and returns everything the server answers until it closes the connection (or goes quiet). */
std::string rawExchange (int port, const std::string& request)
{
    const int fd = ::socket (AF_INET, SOCK_STREAM, 0);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons ((uint16_t) port);
    inet_pton (AF_INET, "127.0.0.1", &a.sin_addr);
    timeval tv { 3, 0 };
    ::setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string reply;

    if (::connect (fd, (sockaddr*) &a, sizeof a) == 0 && ::send (fd, request.data(), request.size(), 0) == (ssize_t) request.size())
    {
        char buf[4096];
        ssize_t n;

        while ((n = ::recv (fd, buf, sizeof buf, 0)) > 0)
            reply.append (buf, (size_t) n);
    }

    ::close (fd);
    return reply;
}
} // namespace

ROVOX_TEST (upload, bodies_without_a_length_get_411_and_every_denial_closes_the_connection)
{
    ServerOptions so;
    so.maxBodyBytes = 1024;
    Fixture f (so);
    const std::string head = "Host: 127.0.0.1\r\nContent-Type: application/json\r\n";

    // no Content-Length (and not chunked): refused before the body is read; the server closes, so an unread body cannot desync keep-alive
    const auto noLength = rawExchange (f.server->port(), "POST /api/login HTTP/1.1\r\n" + head + "\r\n");
    CHECK (noLength.find (" 411 ") != std::string::npos);
    CHECK (noLength.find ("Connection: close") != std::string::npos);

    const auto chunked = rawExchange (f.server->port(), "POST /api/login HTTP/1.1\r\n" + head + "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    CHECK (chunked.find (" 413 ") != std::string::npos);
    CHECK (chunked.find ("Connection: close") != std::string::npos);

    const auto foreign = rawExchange (f.server->port(), "GET /api/projects HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://evil.example\r\n\r\n");
    CHECK (foreign.find (" 403 ") != std::string::npos);
    CHECK (foreign.find ("Connection: close") != std::string::npos);

    const auto wrongType = rawExchange (f.server->port(), "POST /api/projects HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nhi");
    CHECK (wrongType.find (" 415 ") != std::string::npos);
    CHECK (wrongType.find ("Connection: close") != std::string::npos);

    // only the streamed tus PATCH goes without the cap: the (non-streamed) tus POST and DELETE bodies keep it
    auto bigCreate = f.client->Post ("/api/uploads", httplib::Headers { { "Tus-Resumable", "1.0.0" }, { "Upload-Length", "10" } }, std::string (4096, 'a'), "text/plain");
    REQUIRE (bigCreate != nullptr);
    CHECK (bigCreate->status == 413);
    auto bigDelete = f.client->Delete ("/api/uploads/someid", std::string (4096, 'a'), "text/plain");
    REQUIRE (bigDelete != nullptr);
    CHECK (bigDelete->status == 413);
}

ROVOX_TEST (upload, a_render_file_that_shrinks_during_the_download_closes_the_connection)
{
    Fixture f;
    const auto id = f.env.newProject ("shrinking");
    const auto dir = f.env.project (id).rendersDir();
    fs::create_directories (dir);
    const auto file = dir / "big.wav";
    constexpr std::uintmax_t total = 64u << 20;

    {
        std::ofstream (file, std::ios::binary) << "x";
        fs::resize_file (file, total);              // sparse: far more than the socket buffers hold
    }

    size_t received = 0;
    bool truncated = false;
    httplib::Client c ("127.0.0.1", f.server->port());
    c.set_read_timeout (10, 0);
    auto res = c.Get ("/api/projects/" + id + "/renders/big.wav", [&] (const char*, size_t len)
    {
        received += len;

        if (! truncated && received >= (1u << 20))
        {
            truncated = true;
            fs::resize_file (file, 1024);           // the file got shorter while it was served
        }

        return true;
    });

    CHECK (truncated);
    CHECK (received < total);                       // the server closed instead of looping forever on an empty read
    CHECK (! res);
}

ROVOX_TEST (upload, patch_past_upload_length_is_413_and_nothing_extra_is_written)
{
    Fixture f;
    const auto loc = tusCreate (*f.client, 10, "short.bin");
    auto r = f.client->Patch (loc, httplib::Headers { { "Upload-Offset", "0" } }, patternData (64, 5), "application/offset+octet-stream");
    REQUIRE (r != nullptr);
    CHECK (r->status == 413);

    auto head = f.client->Get (loc);
    REQUIRE (head != nullptr);
    CHECK (head->get_header_value ("Upload-Offset") == "0");

    // the writer itself refuses to pass the length
    const auto id = loc.substr (loc.rfind ('/') + 1);
    auto w = f.server->uploads().openWriter (id, 0);
    REQUIRE (w.ok());
    CHECK (w.value()->write ("0123456789", 10));
    CHECK (! w.value()->write ("x", 1));
    CHECK (w.value()->overflowed());
    CHECK (fs::file_size (f.server->uploads().partPath (id)) == 10);
}

ROVOX_TEST (upload, wrong_query_tokens_count_and_a_right_login_is_never_locked_out)
{
    ServerOptions so;
    so.token = "tok-right";
    Fixture f (so);

    for (int i = 0; i < 14; ++i)
        f.client->Get ("/?token=wrong" + std::to_string (i));

    auto guess = f.client->Post ("/api/login", R"({"token":"guess"})", "application/json");
    REQUIRE (guess != nullptr);
    CHECK (guess->status == 429);

    auto right = f.client->Post ("/api/login", R"({"token":"tok-right"})", "application/json");
    REQUIRE (right != nullptr);
    CHECK (right->status == 200);
    CHECK (right->get_header_value ("Set-Cookie").find ("rovox_token=tok-right") != std::string::npos);
}

ROVOX_TEST (upload, hashing_a_finished_upload_does_not_block_other_uploads)
{
    Fixture f;
    const size_t total = 192u << 20;
    const std::string chunk = patternData (1u << 20, 9);
    Sha256 sha;

    for (size_t i = 0; i < total / chunk.size(); ++i)
        sha.update (chunk.data(), chunk.size());

    auto info = f.server->uploads().create (total, { { "filename", "huge.bin" }, { "sha256", sha.hexDigest() } });
    auto other = f.server->uploads().create (10, { { "filename", "small.bin" } });
    REQUIRE (info.ok());
    REQUIRE (other.ok());

    auto w = f.server->uploads().openWriter (info.value().id, 0);
    REQUIRE (w.ok());

    for (size_t i = 0; i < total / chunk.size(); ++i)
        REQUIRE (w.value()->write (chunk.data(), chunk.size()));

    std::atomic<bool> finishing { true };
    std::thread finisher ([&]
    {
        auto done = f.server->uploads().finishPatch (*w.value());
        CHECK (done.ok());
        finishing = false;
    });

    std::this_thread::sleep_for (std::chrono::milliseconds (30));
    auto seen = f.server->uploads().info (other.value().id);
    const bool stillHashing = finishing.load();
    CHECK (seen.ok());
    CHECK (stillHashing);               // the answer came while the big upload was being hashed
    finisher.join();
}

namespace
{
const std::string testBatch = "bt_0123456789abcdef0123456789abcdef";

std::string slurp (const fs::path& p)
{
    std::ifstream in (p, std::ios::binary);
    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
}

/** Uploads every {relpath, bytes} into the store as one batch (relpaths go in unchecked, like a hostile tus client's metadata). */
void uploadFiles (UploadStore& store, const std::string& batch, const std::vector<std::pair<std::string, std::string>>& files)
{
    for (auto& [rel, data] : files)
    {
        auto u = store.create (data.size(), { { "filename", fs::path (rel).filename().string() }, { "relpath", rel }, { "batch_id", batch } });
        REQUIRE (u.ok());
        REQUIRE (store.append (u.value().id, 0, data.data(), data.size()).ok());
    }
}

std::string wavBytes (double hz, double seconds)
{
    const auto p = test::makeTempDir ("upwav") / "x.wav";
    test::writeWav (p, { test::sine (hz, -18.0, seconds, 44100.0) }, 44100.0, 24);
    return slurp (p);
}
} // namespace

ROVOX_TEST (upload, finish_materialises_a_batch_that_import_plan_can_read)
{
    Fixture f;
    auto& store = f.server->uploads();
    uploadFiles (store, testBatch, { { "gtr.wav", wavBytes (220, 1.0) }, { "bass.wav", wavBytes (110, 1.2) }, { "vox.wav", wavBytes (440, 0.9) } });

    auto status = store.batchStatus (testBatch);
    REQUIRE (status.ok());
    CHECK (! status.value().contains ("materialised_path"));

    auto r = store.finish (testBatch);
    REQUIRE (r.ok());
    const auto path = fs::path (r.value()["path"].get<std::string>());
    CHECK (r.value()["detected"] == "audio");
    CHECK (r.value()["files"].size() == 3);
    CHECK (fs::exists (path / "gtr.wav"));
    CHECK (fs::is_regular_file (path / "bass.wav"));
    CHECK (r.value()["files"][0]["sha256"].get<std::string>() == sha256Hex (slurp (path / r.value()["files"][0]["relpath"].get<std::string>())));

    status = store.batchStatus (testBatch);
    REQUIRE (status.ok());
    CHECK (status.value()["materialised_path"].get<std::string>() == path.string());

    const auto id = f.env.newProject ("up");
    auto plan = f.env.ok (id, "import.plan", { { "paths", { path.string() } } });
    REQUIRE (plan["tracks"].size() == 3);
    std::set<std::string> names;

    for (auto& t : plan["tracks"])
        names.insert (t["clips"][0]["file"].get<std::string>());

    CHECK ((names == std::set<std::string> { "gtr.wav", "bass.wav", "vox.wav" }));
}

ROVOX_TEST (upload, finish_is_idempotent_and_refuses_unfinished_batches)
{
    Fixture f;
    auto& store = f.server->uploads();
    auto pending = store.create (10, { { "filename", "a.wav" }, { "relpath", "a.wav" }, { "batch_id", testBatch } });
    REQUIRE (pending.ok());
    CHECK (! store.finish (testBatch).ok());

    REQUIRE (store.append (pending.value().id, 0, "0123456789", 10).ok());
    auto first = store.finish (testBatch);
    auto second = store.finish (testBatch);
    REQUIRE (first.ok());
    REQUIRE (second.ok());
    CHECK (first.value() == second.value());
    CHECK (! store.finish ("bt_zz").ok());
}

ROVOX_TEST (upload, finish_detects_a_zoom_r20_project_folder)
{
    Fixture f;
    auto& store = f.server->uploads();
    const auto folder = test::makeR20Folder (test::makeTempDir ("r20up"), "ZOOM0001.zprj", 3, 1.0);
    std::vector<std::pair<std::string, std::string>> files;

    for (auto& e : fs::recursive_directory_iterator (folder))
        if (e.is_regular_file())
            files.push_back ({ "ZOOM0001.zprj/" + fs::relative (e.path(), folder).generic_string(), slurp (e.path()) });

    uploadFiles (store, testBatch, files);
    auto r = store.finish (testBatch);
    REQUIRE (r.ok());
    CHECK (r.value()["detected"] == "zoom_r20");
    const auto path = fs::path (r.value()["path"].get<std::string>());
    CHECK (path.filename() == "ZOOM0001.zprj");
    CHECK (fs::exists (path / "R20PRJDATA.r20prj"));

    const auto id = f.env.newProject ("r20up");
    auto plan = f.env.ok (id, "import.plan", { { "paths", { path.string() } } });
    CHECK (plan["detected_source"] == "zoom_r20");
    CHECK (plan["tracks"].size() == 3);
    CHECK (plan["tracks"][0]["name"] == "REC01");

    const auto dir = store.materialisedDir (testBatch);
    f.env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    CHECK (! fs::exists (dir));                     // the media is in the project now; the upload copy is gone
}

ROVOX_TEST (upload, finish_neutralises_hostile_relpaths_and_resolves_collisions)
{
    Fixture f;
    auto& store = f.server->uploads();
    const std::string withControl = std::string ("ctl") + '\x01' + '\n' + "x.wav";
    uploadFiles (store, testBatch, { { "../../evil.wav", "1" }, { "/etc/passwd", "2" }, { "a//b/./../c.wav", "3" }, { "dup.wav", "4" }, { "dup.wav", "5" },
                                     { withControl, "6" }, { std::string (400, 'n') + ".wav", "7" } });
    auto r = store.finish (testBatch);
    REQUIRE (r.ok());
    const auto dir = store.materialisedDir (testBatch);
    std::set<std::string> rels;

    for (auto& file : r.value()["files"])
    {
        const auto rel = file["relpath"].get<std::string>();
        rels.insert (rel);
        CHECK (rel.find ("..") == std::string::npos);
        CHECK (rel[0] != '/');
        CHECK (fs::weakly_canonical (dir / rel).string().rfind (fs::weakly_canonical (dir).string(), 0) == 0);

        for (auto& seg : fs::path (rel))
            CHECK (seg.string().size() <= 255);
    }

    CHECK (rels.count ("evil.wav") == 1);
    CHECK (rels.count ("etc/passwd") == 1);
    CHECK (rels.count ("a/b/c.wav") == 1);
    CHECK (rels.count ("dup.wav") == 1);
    CHECK (rels.count ("dup (2).wav") == 1);
    CHECK (rels.count ("ctlx.wav") == 1);
    CHECK (! fs::exists (dir.parent_path() / "evil.wav"));
    CHECK (! fs::exists (dir.parent_path().parent_path() / "evil.wav"));
}

ROVOX_TEST (upload, a_single_upload_without_a_batch_finishes_to_its_filename)
{
    Fixture f;
    auto& store = f.server->uploads();
    auto u = store.create (3, { { "filename", "take 1.wav" } });
    REQUIRE (u.ok());
    REQUIRE (store.append (u.value().id, 0, "abc", 3).ok());
    auto r = store.finish (u.value().id);
    REQUIRE (r.ok());
    CHECK (r.value()["files"][0]["relpath"] == "take 1.wav");
    CHECK (r.value()["files"][0]["sha256"].get<std::string>() == sha256Hex ("abc"));
    CHECK (slurp (store.materialisedDir (u.value().id) / "take 1.wav") == "abc");
}

ROVOX_TEST (upload, materialised_batches_expire_after_the_ttl_from_completion)
{
    UploadLimits limits;
    limits.ttl = std::chrono::seconds (60);
    UploadStore store (test::makeTempDir ("store"), limits);
    uploadFiles (store, testBatch, { { "x/a.wav", "aaaa" }, { "x/b.wav", "bbbb" } });
    REQUIRE (store.finish (testBatch).ok());
    const auto dir = store.materialisedDir (testBatch);
    REQUIRE (fs::exists (dir));

    CHECK (store.cleanup (std::time (nullptr)) == 0);
    CHECK (fs::exists (dir));
    CHECK (store.cleanup (std::time (nullptr) + 3600) == 2);
    CHECK (! fs::exists (dir));
}
