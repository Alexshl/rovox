// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "HttpServer.h"

#include "Ids.h"
#include "Sha1.h"
#include "Sha256.h"
#include "Version.h"

#include <httplib.h>

#include <fstream>

namespace rovox
{

namespace
{
int statusFor (const std::string& code)
{
    if (code == errc::invalidParams)        return 400;
    if (code == errc::notFound)             return 404;
    if (code == errc::revisionConflict)     return 409;
    if (code == errc::preconditionFailed)   return 412;
    if (code == errc::preflightFailed)      return 422;
    if (code == errc::busy)                 return 423;
    if (code == errc::unsupported)          return 501;
    return 500;
}

void reply (httplib::Response& res, int status, const json& body)
{
    res.status = status;
    res.set_content (body.dump(), "application/json");
}

void replyError (httplib::Response& res, const Error& e)
{
    reply (res, statusFor (e.code), { { "ok", false }, { "error", e.toJson() } });
}

/** tus "Upload-Metadata: key base64value,key2 base64value2" */
std::map<std::string, std::string> parseTusMetadata (const std::string& header)
{
    std::map<std::string, std::string> out;
    size_t pos = 0;

    while (pos < header.size())
    {
        auto end = header.find (',', pos);
        const auto item = header.substr (pos, end == std::string::npos ? std::string::npos : end - pos);
        const auto space = item.find (' ');
        const auto key = item.substr (0, space);

        if (! key.empty())
            out[key] = space == std::string::npos ? std::string() : base64Decode (item.substr (space + 1));

        if (end == std::string::npos)
            break;

        pos = end + 1;
    }

    return out;
}

void tusHeaders (httplib::Response& res)
{
    res.set_header ("Tus-Resumable", "1.0.0");
}

std::string safeFileName (const std::string& n)
{
    return (n.empty() || n.find ('/') != std::string::npos || n.find ('\\') != std::string::npos || n.find ("..") != std::string::npos) ? std::string() : n;
}
} // namespace

HttpServer::HttpServer (EngineHost& host, ProjectManager& pm, CommandBus& bus, EventHub& hub, ServerOptions o)
    : host_ (host), projects_ (pm), bus_ (bus), hub_ (hub), options_ (std::move (o))
{
    UploadStore::Listener listener;
    listener.onCompleted = [this] (const UploadInfo& u) { hub_.publish ("local", "", { { "type", "upload.completed" }, { "upload", u.toJson() } }); };
    listener.onBatchCompleted = [this] (const std::string& id) { hub_.publish ("local", "", { { "type", "upload.batch_completed" }, { "batch_id", id } }); };
    uploads_ = std::make_unique<UploadStore> (host_.dataDir() / "uploads", options_.uploadLimits, std::move (listener));
    server_ = std::make_unique<httplib::Server>();
}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::mount (std::function<void (httplib::Server&)> fn)
{
    mounts_.push_back (std::move (fn));
}

Result<void> HttpServer::start()
{
    installRoutes();

    for (auto& m : mounts_)
        m (*server_);

    // long-poll tool calls and SSE streams each hold a worker; the default pool (8) would starve ordinary requests
    server_->new_task_queue = [] { return new httplib::ThreadPool (64); };

    if (options_.port == 0)
        port_ = server_->bind_to_any_port (options_.bind.c_str());
    else if (server_->bind_to_port (options_.bind.c_str(), options_.port))
        port_ = options_.port;

    if (port_ <= 0)
        return makeError (errc::io, "cannot listen on " + options_.bind + ":" + std::to_string (options_.port) + " (address in use or not available)");

    thread_ = std::thread ([this] { server_->listen_after_bind(); });
    return {};
}

void HttpServer::stop()
{
    if (server_ != nullptr)
        server_->stop();

    if (thread_.joinable())
        thread_.join();
}

void HttpServer::installRoutes()
{
    auto& svr = *server_;
    svr.set_payload_max_length (std::numeric_limits<size_t>::max());      // uploads are streamed; the per-file limit is enforced by the UploadStore
    svr.set_read_timeout (60, 0);
    svr.set_keep_alive_max_count (200);

    svr.set_pre_routing_handler ([this] (const httplib::Request& req, httplib::Response& res)
    {
        if (req.path == "/healthz" || options_.token.empty() || req.method == "OPTIONS")
            return httplib::Server::HandlerResponse::Unhandled;

        const auto auth = req.get_header_value ("Authorization");
        const bool ok = auth == "Bearer " + options_.token || (req.method == "GET" && req.get_param_value ("token") == options_.token);

        if (ok)
            return httplib::Server::HandlerResponse::Unhandled;

        reply (res, 401, { { "ok", false }, { "error", { { "code", "UNAUTHORIZED" }, { "message", "missing or wrong token" } } } });
        return httplib::Server::HandlerResponse::Handled;
    });

    svr.Get ("/healthz", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, 200, { { "status", "ok" }, { "version", rovoxVersion() }, { "tracktion", tracktionVersion() },
                           { "uptime_s", std::chrono::duration<double> (std::chrono::steady_clock::now() - started_).count() },
                           { "audio", toString (host_.audioMode()) }, { "cpus", host_.cpuBudget().cpus } });
    });

    auto runCommand = [this] (const std::string& name, const json& params, CommandContext ctx, httplib::Response& res)
    {
        auto future = bus_.submit (name, params, ctx);

        if (future.wait_for (std::chrono::seconds (120)) != std::future_status::ready)
        {
            reply (res, 504, { { "ok", false }, { "error", { { "code", errc::busy }, { "message", "the message thread did not answer in time" } } } });
            return;
        }

        auto result = future.get();
        reply (res, result.ok ? 200 : (result.error ? statusFor (result.error->code) : 500), result.toJson());
    };

    auto contextFrom = [] (const json& body, const std::string& project)
    {
        CommandContext ctx;
        ctx.projectId = project;
        ctx.origin = { Origin::Kind::mcp, "http", "http" };

        if (body.contains ("origin") && body["origin"].is_object())
        {
            const auto kind = body["origin"].value ("kind", "mcp");
            ctx.origin.kind = kind == "ui" ? Origin::Kind::ui : (kind == "skill" ? Origin::Kind::skill : Origin::Kind::mcp);
            ctx.origin.session = body["origin"].value ("session", "http");
            ctx.origin.label = body["origin"].value ("label", "http");
        }

        if (body.contains ("if_revision") && body["if_revision"].is_number_unsigned())
            ctx.ifRevision = body["if_revision"].get<std::uint64_t>();

        ctx.undoLabel = body.value ("undo_label", "");
        ctx.dryRun = body.value ("dry_run", false);
        return ctx;
    };

    svr.Get ("/api/projects", [runCommand] (const httplib::Request&, httplib::Response& res)
    {
        runCommand ("project.list", json::object(), {}, res);
    });

    svr.Post ("/api/projects", [runCommand] (const httplib::Request& req, httplib::Response& res)
    {
        const auto body = json::parse (req.body.empty() ? "{}" : req.body, nullptr, false);

        if (body.is_discarded())
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "body is not JSON" } } } });

        runCommand ("project.new", body, {}, res);
    });

    svr.Get (R"(/api/projects/([^/]+))", [runCommand] (const httplib::Request& req, httplib::Response& res)
    {
        CommandContext ctx;
        ctx.projectId = req.matches[1];
        runCommand ("project.info", json::object(), ctx, res);
    });

    svr.Post (R"(/api/projects/([^/]+)/commands)", [runCommand, contextFrom] (const httplib::Request& req, httplib::Response& res)
    {
        const auto body = json::parse (req.body, nullptr, false);

        if (body.is_discarded() || ! body.is_object() || ! body.contains ("name") || ! body["name"].is_string())
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "body must be {\"name\": ..., \"params\": {...}}" } } } });

        runCommand (body["name"].get<std::string>(), body.value ("params", json::object()), contextFrom (body, req.matches[1]), res);
    });

    for (const char* dirName : { "renders", "previews" })
    {
        const std::string dir = dirName;
        svr.Get ("/api/projects/([^/]+)/" + dir + "/([^/]+)", [this, dir] (const httplib::Request& req, httplib::Response& res)
        {
            const auto name = safeFileName (req.matches[2]);
            std::optional<fs::path> projectDir;      // the mutex-guarded snapshot: this runs on an HTTP thread, not the message thread

            for (auto& summary : projects_.list ("local", true))
                if (summary.id == req.matches[1])
                    projectDir = summary.dir;

            if (! projectDir || name.empty() || name[0] == '.')
                return reply (res, 404, { { "ok", false }, { "error", { { "code", errc::notFound }, { "message", "no such file" } } } });

            const auto file = *projectDir / dir / name;
            std::ifstream in (file, std::ios::binary);

            if (! in)
                return reply (res, 404, { { "ok", false }, { "error", { { "code", errc::notFound }, { "message", "no such file" } } } });

            const auto size = fs::file_size (file);
            const auto ext = file.extension().string();
            const std::string mime = ext == ".mp3" ? "audio/mpeg" : (ext == ".flac" ? "audio/flac" : (ext == ".wav" ? "audio/wav" : "application/octet-stream"));
            res.set_content_provider (size, mime, [file] (size_t offset, size_t length, httplib::DataSink& sink)
            {
                std::ifstream f (file, std::ios::binary);
                f.seekg ((std::streamoff) offset);
                std::string buf (std::min<size_t> (length, 1 << 20), '\0');
                f.read (buf.data(), (std::streamsize) buf.size());
                sink.write (buf.data(), (size_t) f.gcount());
                return true;
            });
        });
    }

    //==========================================================================
    // tus 1.0 uploads: core + creation + checksum + termination
    svr.Options ("/api/uploads", [this] (const httplib::Request&, httplib::Response& res)
    {
        tusHeaders (res);
        res.set_header ("Tus-Version", "1.0.0");
        res.set_header ("Tus-Extension", "creation,checksum,termination");
        res.set_header ("Tus-Checksum-Algorithm", "sha1,sha256");
        res.set_header ("Tus-Max-Size", std::to_string (options_.uploadLimits.maxFileBytes));
        res.status = 204;
    });

    svr.Post ("/api/uploads", [this] (const httplib::Request& req, httplib::Response& res)
    {
        tusHeaders (res);

        if (! req.has_header ("Upload-Length"))
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "Upload-Length is required" } } } });

        std::uint64_t length = 0;

        try { length = std::stoull (req.get_header_value ("Upload-Length")); }
        catch (...) { return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "bad Upload-Length" } } } }); }

        auto created = uploads_->create (length, parseTusMetadata (req.get_header_value ("Upload-Metadata")));

        if (! created.ok())
            return replyError (res, created.error());

        res.set_header ("Location", "/api/uploads/" + created.value().id);
        res.set_header ("Upload-Offset", "0");
        res.status = 201;
    });

    svr.Get (R"(/api/uploads/([^/]+))", [this] (const httplib::Request& req, httplib::Response& res)
    {
        tusHeaders (res);
        auto u = uploads_->info (req.matches[1]);

        if (! u.ok())
        {
            res.status = 404;
            return;
        }

        res.set_header ("Upload-Offset", std::to_string (u.value().offset));
        res.set_header ("Upload-Length", std::to_string (u.value().length));
        res.set_header ("Cache-Control", "no-store");
        res.status = 200;
    });

    svr.Delete (R"(/api/uploads/([^/]+))", [this] (const httplib::Request& req, httplib::Response& res)
    {
        tusHeaders (res);
        auto r = uploads_->terminate (req.matches[1]);
        res.status = r.ok() ? 204 : 404;
    });

    svr.Patch (R"(/api/uploads/([^/]+))", [this] (const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader)
    {
        tusHeaders (res);

        if (req.get_header_value ("Content-Type") != "application/offset+octet-stream")
        {
            res.status = 415;
            return;
        }

        std::uint64_t offset = 0;

        try { offset = std::stoull (req.get_header_value ("Upload-Offset")); }
        catch (...) { res.status = 400; return; }

        auto writer = uploads_->openWriter (req.matches[1], offset);

        if (! writer.ok())
        {
            const auto& code = writer.error().code;
            res.status = code == errc::notFound ? 404 : (code == errc::preconditionFailed ? 409 : (code == errc::busy ? 423 : 400));

            if (writer.error().details.contains ("offset"))
                res.set_header ("Upload-Offset", std::to_string (writer.error().details["offset"].get<std::uint64_t>()));

            return;
        }

        // optional per-chunk checksum: "sha1 <base64>" or "sha256 <base64>"
        const auto checksum = req.get_header_value ("Upload-Checksum");
        const auto space = checksum.find (' ');
        const auto algorithm = checksum.substr (0, space);
        Sha1 sha1;
        Sha256 sha256;
        bool ioOk = true;

        reader ([&] (const char* data, size_t len)
        {
            if (algorithm == "sha1")        sha1.update (data, len);
            else if (algorithm == "sha256") sha256.update (data, len);

            ioOk = ioOk && writer.value()->write (data, len);
            return ioOk;
        });

        if (! ioOk)
        {
            writer.value()->rollback();
            res.status = 500;
            return;
        }

        if (! checksum.empty())
        {
            std::string actual;

            if (algorithm == "sha1")
            {
                const auto d = sha1.digest();
                actual = base64Encode (d.data(), d.size());
            }
            else if (algorithm == "sha256")
            {
                const auto d = sha256.digest();
                actual = base64Encode (d.data(), d.size());
            }
            else
            {
                writer.value()->rollback();
                res.status = 400;
                return;
            }

            if (actual != checksum.substr (space + 1))
            {
                writer.value()->rollback();
                res.status = 460;               // Checksum Mismatch (tus)
                return;
            }
        }

        auto done = uploads_->finishPatch (*writer.value());

        if (! done.ok())
        {
            writer.value()->rollback();
            return replyError (res, done.error());
        }

        res.set_header ("Upload-Offset", std::to_string (done.value().offset));
        res.status = 204;
    });

    svr.Post ("/api/upload-batches", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto body = json::parse (req.body, nullptr, false);

        if (body.is_discarded() || ! body.contains ("files"))
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "body must be {\"files\": [{\"relpath\", \"size\"}]}" } } } });

        auto batch = uploads_->createBatch (body["files"]);
        batch.ok() ? reply (res, 201, batch.value()) : replyError (res, batch.error());
    });

    svr.Get (R"(/api/upload-batches/([^/]+))", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto status = uploads_->batchStatus (req.matches[1]);
        status.ok() ? reply (res, 200, status.value()) : replyError (res, status.error());
    });
}

} // namespace rovox
