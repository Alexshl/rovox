// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "HttpServer.h"

#include "Gateway.h"
#include "Ids.h"
#include "JobFiles.h"
#include "OpenLink.h"
#include "Peaks.h"
#include "Quota.h"
#include "RequestGuard.h"
#include "Scopes.h"
#include "Sha1.h"
#include "Sha256.h"
#include "Version.h"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <ctime>
#include <fstream>
#include <limits>

#include <unistd.h>

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
    if (code == errc::forbiddenPath)        return 403;
    if (code == errc::forbiddenLicense)     return 403;
    if (code == errc::unauthorized)         return 401;
    if (code == errc::forbiddenScope)       return 403;
    if (code == errc::quotaExceeded)        return 429;
    if (code == errc::rateLimited)          return 429;
    return 500;
}

void reply (httplib::Response& res, int status, const json& body)
{
    res.status = status;
    res.set_content (body.dump (-1, ' ', false, json::error_handler_t::replace), "application/json");
}

/** Retry-After for a 429, always: `retry_after_s` of the details, else the seconds until `resets_at` (ceil, at least 1), else 60. */
void setRetryAfter (httplib::Response& res, const Error& e)
{
    double seconds = 60;

    if (e.details.is_object() && e.details.contains ("retry_after_s") && e.details["retry_after_s"].is_number())
    {
        seconds = e.details["retry_after_s"].get<double>();
    }
    else if (e.details.is_object() && e.details.contains ("resets_at") && e.details["resets_at"].is_string())
    {
        if (const auto at = parseIsoUtc (e.details["resets_at"].get<std::string>()))
            seconds = *at - (double) std::time (nullptr);
    }

    res.set_header ("Retry-After", std::to_string (std::max (1, (int) std::ceil (seconds))));
}

void replyError (httplib::Response& res, const Error& e)
{
    if (statusFor (e.code) == 429)
        setRetryAfter (res, e);

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

std::string sessionCookie (const std::string& signedValue, int maxAgeS = 2592000)
{
    return "rovox_session=" + signedValue + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string (maxAgeS);
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
    if (options_.authenticator != nullptr)
    {
        auth_ = options_.authenticator;
    }
    else
    {
        ownAuth_ = std::make_unique<Authenticator> (nullptr, options_.token);
        auth_ = ownAuth_.get();

        if (options_.sessionKeyFile.empty())
            options_.sessionKeyFile = host_.dataDir() / "auth" / "session.key";
    }

    if (! options_.sessionKeyFile.empty())
        auth_->setSessionKeyFile (options_.sessionKeyFile);

    UploadStore::Listener listener;
    listener.onCompleted = [this] (const UploadInfo& u) { hub_.publish (u.tenant, "", { { "type", "upload.completed" }, { "upload", u.toJson() } }); };
    listener.onBatchCompleted = [this] (const std::string& id)
    {
        const auto status = uploads_->batchStatus (id);         // the operator's view: only to learn which tenant owns the batch
        hub_.publish (status.ok() ? status.value().value ("tenant", "local") : std::string ("local"), "", { { "type", "upload.batch_completed" }, { "batch_id", id } });
    };
    uploads_ = std::make_unique<UploadStore> (host_.dataDir() / "uploads", options_.uploadLimits, std::move (listener));
    uploads_->setTenantBase (host_.dataDir());
    server_ = makeServerWithUpgrade ([this] (int fd, WsRequest req)
    {
        const auto header = [&req] (const char* name) { auto it = req.headers.find (name); return it == req.headers.end() ? std::string() : it->second; };

        if (! guard::requestAllowed (guard::isLoopbackBind (options_.bind), header ("origin"), header ("host"), options_.allowedOrigins, options_.allowedHosts))
        {
            wsSendHttp (fd, 403, "Forbidden", "Origin or Host not allowed");
            ::close (fd);
        }
        else if (auto* g = gateway_.load())
        {
            g->handleUpgrade (fd, std::move (req));
        }
        else
        {
            wsSendHttp (fd, 404, "Not Found", "the WebSocket gateway is not running");
            ::close (fd);
        }
    });
}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::noteAuthFailure()
{
    std::lock_guard<std::mutex> lock (authMutex_);
    authFailures_.push_back (std::chrono::steady_clock::now());

    while (authFailures_.size() > 64)
        authFailures_.pop_front();
}

/** More than 10 wrong tokens within a minute: further attempts get 429 until the window has cleared. */
bool HttpServer::tooManyAuthFailures()
{
    std::lock_guard<std::mutex> lock (authMutex_);
    const auto now = std::chrono::steady_clock::now();

    while (! authFailures_.empty() && now - authFailures_.front() > std::chrono::seconds (60))
        authFailures_.pop_front();

    return authFailures_.size() > 10;
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
    svr.set_payload_max_length (std::numeric_limits<size_t>::max());      // uploads are streamed (per-file limit: UploadStore); every other route is capped in the pre-routing handler, before the body is read
    svr.set_read_timeout (60, 0);
    svr.set_keep_alive_max_count (200);

    // SharedArrayBuffer / AudioWorklet need a cross-origin isolated page
    svr.set_default_headers ({ { "Cross-Origin-Opener-Policy", "same-origin" }, { "Cross-Origin-Embedder-Policy", "require-corp" },
                               { "Cross-Origin-Resource-Policy", "same-origin" }, { "X-Content-Type-Options", "nosniff" } });

    // the caller of a request that passed pre-routing (resolved again here, cheap); answers 401 itself when there is none
    auto principalFor = [this] (const httplib::Request& req, httplib::Response& res) -> std::optional<Principal>
    {
        auto r = auth_->resolve (authRequestOf (req));

        if (r.ok())
            return r.value();

        replyError (res, r.error());
        return std::nullopt;
    };

    svr.set_pre_routing_handler ([this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto deny = [&res] (int status, const char* code, const char* message)
        {
            reply (res, status, { { "ok", false }, { "error", { { "code", code }, { "message", message } } } });
            res.set_header ("Connection", "close");         // a body we did not read must not be parsed as the next request on a kept-alive connection
            return httplib::Server::HandlerResponse::Handled;
        };

        if (req.method == "OPTIONS" || req.path == "/healthz" || req.path == "/readyz")
            return httplib::Server::HandlerResponse::Unhandled;

        const bool uploadPath = req.path.rfind ("/api/uploads", 0) == 0;
        const bool apiPath = req.path.rfind ("/api/", 0) == 0;

        // bodies are bounded before anything reads them (login is public); only the streamed tus PATCH goes without a cap
        // (the tus POST / DELETE bodies keep the normal one)
        if (! (uploadPath && req.method == "PATCH"))
        {
            const auto length = req.get_header_value ("Content-Length");
            const bool chunked = req.get_header_value ("Transfer-Encoding").find ("chunked") != std::string::npos;
            unsigned long long declared = 0;

            try { declared = length.empty() ? 0 : std::stoull (length); }
            catch (...) { declared = std::numeric_limits<unsigned long long>::max(); }

            if (chunked || declared > options_.maxBodyBytes)
                return deny (413, "PAYLOAD_TOO_LARGE", "request body is too large");

            // without a length the server could read the body until EOF: refuse before reading
            if (length.empty() && (req.method == "POST" || req.method == "PUT" || req.method == "PATCH"))
                return deny (411, "LENGTH_REQUIRED", "Content-Length is required");
        }

        if (apiPath)
        {
            // CSRF and DNS rebinding: always, also without a token. Non-browser clients send no Origin.
            if (! guard::requestAllowed (guard::isLoopbackBind (options_.bind), req.get_header_value ("Origin"), req.get_header_value ("Host"), options_.allowedOrigins, options_.allowedHosts))
                return deny (403, "FORBIDDEN_ORIGIN", "Origin or Host not allowed");

            // a "simple" cross-origin POST (text/plain, form) must not reach the JSON handlers
            if (! uploadPath && (req.method == "POST" || req.method == "PUT" || req.method == "PATCH"))
            {
                auto type = req.get_header_value ("Content-Type");
                std::transform (type.begin(), type.end(), type.begin(), [] (unsigned char c) { return (char) std::tolower (c); });

                if (type.rfind ("application/json", 0) != 0)
                    return deny (415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
            }
        }

        if (auth_->open())
            return httplib::Server::HandlerResponse::Unhandled;

        auto resolved = auth_->resolve (authRequestOf (req));

        // opening the printed URL http://host:4810/?token=... sets the session cookie and drops the token from the address bar
        if (req.path == "/" && req.method == "GET" && req.has_param ("token"))
        {
            AuthRequest viaQuery;
            viaQuery.queryToken = req.get_param_value ("token");

            if (auto r = auth_->resolve (viaQuery); r.ok())
            {
                res.set_header ("Set-Cookie", sessionCookie (auth_->issueSession (r.value())));
                res.set_redirect ("/");
                return httplib::Server::HandlerResponse::Handled;
            }
        }

        const bool protectedPath = req.path.rfind ("/api/", 0) == 0 || req.path == "/mcp" || req.path.rfind ("/mcp/", 0) == 0;

        // a wrong ?token= counts like a wrong header or cookie, also on the page URL
        if (! resolved.ok() && req.has_param ("token") && ! protectedPath && req.path != "/open")
            noteAuthFailure();         // /open counts its own failures

        if (! protectedPath || req.path == "/api/login" || req.path == "/api/session")
            return httplib::Server::HandlerResponse::Unhandled;     // the static shell and the login endpoint are public

        if (resolved.ok())
        {
            // MCP POSTs are rate limited in McpHttp (which answers in JSON-RPC); counting them here too would halve the limit
            // tus PATCH chunks are one logical upload, bounded by the upload limits: not one request each against the rate
            if (! apiPath || (uploadPath && req.method == "PATCH"))
                return httplib::Server::HandlerResponse::Unhandled;

            const auto decision = quotas().checkRate (resolved.value());

            if (decision.allowed)
                return httplib::Server::HandlerResponse::Unhandled;

            const auto error = QuotaService::rateLimitedError (decision);
            setRetryAfter (res, error);
            reply (res, 429, { { "ok", false }, { "error", error.toJson() } });
            return httplib::Server::HandlerResponse::Handled;
        }

        if (tooManyAuthFailures())
        {
            res.set_header ("Retry-After", "30");
            reply (res, 429, { { "ok", false }, { "error", { { "code", "RATE_LIMITED" }, { "message", "too many wrong tokens, try again in a minute" } } } });
            return httplib::Server::HandlerResponse::Handled;
        }

        if (! req.get_header_value ("Authorization").empty() || ! req.get_header_value ("Cookie").empty() || req.has_param ("token"))
            noteAuthFailure();

        if (const auto challenge = auth_->challengeHeader(); ! challenge.empty())
            res.set_header ("WWW-Authenticate", challenge);

        reply (res, 401, { { "ok", false }, { "error", { { "code", "UNAUTHORIZED" }, { "message", "missing or wrong token" } } } });
        return httplib::Server::HandlerResponse::Handled;
    });

    // login for the web UI: POST {"token": "<server token or rvx_ key>"} -> HttpOnly SameSite=Strict signed session cookie (never the token itself)
    svr.Post ("/api/login", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto body = json::parse (req.body.empty() ? "{}" : req.body, nullptr, false);
        const auto given = body.is_object() && body.contains ("token") && body["token"].is_string() ? body["token"].get<std::string>() : std::string();

        // the token is checked first: a right token always logs in, even while wrong guesses are being rate limited
        const auto presented = "Bearer " + given;
        auto principal = auth_->resolve (std::string_view (presented));

        if (! principal.ok())
        {
            if (tooManyAuthFailures())
            {
                res.set_header ("Retry-After", "30");
                return reply (res, 429, { { "ok", false }, { "error", { { "code", "RATE_LIMITED" }, { "message", "too many wrong tokens, try again in a minute" } } } });
            }

            noteAuthFailure();
            return reply (res, 401, { { "ok", false }, { "error", { { "code", "UNAUTHORIZED" }, { "message", "wrong token" } } } });
        }

        if (const auto cookie = auth_->issueSession (principal.value()); ! cookie.empty())
            res.set_header ("Set-Cookie", sessionCookie (cookie));

        reply (res, 200, { { "ok", true } });
    });

    // one-time signed link into the web UI (E9-01, see OpenLink.h): the platform mints the token, a valid one starts a session of its tenant and subject
    auto openLinks = std::make_shared<OpenLinks> (host_.dataDir(), auth_->configFile());
    svr.Get ("/open", [this, openLinks] (const httplib::Request& req, httplib::Response& res)
    {
        res.set_header ("Cache-Control", "no-store");
        res.set_header ("Referrer-Policy", "no-referrer");     // the token is in the address of this request
        const auto refuse = [&] (int status, const char* code, const char* message)
        {
            reply (res, status, { { "ok", false }, { "error", { { "code", code }, { "message", message } } } });
        };

        if (tooManyAuthFailures())
        {
            res.set_header ("Retry-After", "30");
            return refuse (429, "RATE_LIMITED", "too many wrong links, try again in a minute");
        }

        const auto project = req.get_param_value ("project");
        const auto token = req.get_param_value ("token");
        const bool plainProject = ! project.empty() && project.size() <= 64
            && std::all_of (project.begin(), project.end(), [] (char c) { return std::isalnum ((unsigned char) c) != 0 || c == '_' || c == '-'; });
        const auto now = (std::int64_t) std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count();
        auto grant = plainProject && ! token.empty() ? openLinks->redeem (token, project, now) : Result<OpenGrant> (makeError (errc::unauthorized, "bad link"));

        if (! grant.ok())
        {
            noteAuthFailure();
            return refuse (403, "FORBIDDEN", "the link is invalid, expired or already used");
        }

        Principal p;
        p.tenant = grant.value().tenant;
        p.subject = grant.value().subject;
        p.client = grant.value().client;
        p.scopes = grant.value().scopes;
        constexpr int sessionS = 8 * 3600;
        res.set_header ("Set-Cookie", sessionCookie (auth_->issueOpenSession (p, sessionS), sessionS));
        res.set_redirect ("/?project=" + project);
    });

    svr.Get ("/api/session", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto principal = auth_->resolve (authRequestOf (req));
        json body { { "token_required", ! auth_->open() }, { "authenticated", principal.ok() }, { "version", rovoxVersion() }, { "commit", rovoxGitSha() },
                    { "source_url", options_.sourceUrl }, { "audio", toString (host_.audioMode()) } };

        if (principal.ok())
        {
            const auto& p = principal.value();
            body["tenant"] = p.tenant;
            body["key_id"] = p.keyId;
            body["scopes"] = p.scopes.names();
            body["auth_mode"] = p.authMode;
        }

        reply (res, 200, body);
    });

    // AGPL section 13: where the running source code can be obtained, with the commit this binary was built from
    svr.Get ("/source", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, 200, { { "license", "AGPL-3.0-or-later" }, { "source_url", options_.sourceUrl }, { "version", rovoxVersion() }, { "commit", rovoxGitSha() } });
    });

    if (! options_.webDir.empty() && fs::is_directory (options_.webDir))
    {
        svr.set_file_extension_and_mimetype_mapping ("woff2", "font/woff2");
        svr.set_file_extension_and_mimetype_mapping ("mjs", "text/javascript");
        svr.set_file_extension_and_mimetype_mapping ("map", "application/json");
        svr.set_mount_point ("/", options_.webDir.string());
        svr.set_file_request_handler ([] (const httplib::Request& req, httplib::Response& res)
        {
            res.set_header ("Cache-Control", req.path.rfind ("/assets/", 0) == 0 ? "public, max-age=31536000, immutable" : "no-cache");
            // the built app has no inline script or style attribute: scripts, styles and workers only from this origin
            res.set_header ("Content-Security-Policy",
                            "default-src 'self'; script-src 'self'; style-src 'self'; font-src 'self' data:; img-src 'self' data: blob:; media-src 'self' blob:; "
                            "connect-src 'self' ws: wss:; worker-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'; object-src 'none'");
            res.set_header ("X-Content-Type-Options", "nosniff");
            res.set_header ("Referrer-Policy", "no-referrer");
        });
    }

    // min/max peaks of an item, for the waveform drawing: GET /api/projects/{id}/peaks/{item_id}?bins=N&t0=..&t1=..  (t relative to the item)
    svr.Get (R"(/api/projects/([^/]+)/peaks/([^/]+))", [this, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        CommandContext ctx;
        ctx.projectId = req.matches[1];
        ctx.origin = { Origin::Kind::ui, "http", "peaks" };
        ctx.setPrincipal (*principal);
        auto future = bus_.submit ("item.source", { { "item_id", req.matches[2].str() } }, ctx);

        if (future.wait_for (std::chrono::seconds (30)) != std::future_status::ready)
            return reply (res, 504, { { "ok", false }, { "error", { { "code", errc::busy }, { "message", "the message thread did not answer in time" } } } });

        auto source = future.get();

        if (! source.ok)
            return replyError (res, source.error.value_or (makeError (errc::internal, "item.source failed")));

        const double length = source.data["length_s"].get<double>(), offset = source.data["offset_s"].get<double>(), speed = source.data.value ("speed", 1.0);
        double t0 = 0, t1 = length;
        int bins = 512;

        try
        {
            if (req.has_param ("t0")) t0 = std::stod (req.get_param_value ("t0"));
            if (req.has_param ("t1")) t1 = std::stod (req.get_param_value ("t1"));
            if (req.has_param ("bins")) bins = std::stoi (req.get_param_value ("bins"));
        }
        catch (...)
        {
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "t0, t1 and bins must be numbers" } } } });
        }

        t0 = std::clamp (t0, 0.0, length);
        t1 = std::clamp (t1, t0, length);
        bins = std::clamp (bins, 1, 8192);

        auto peaks = peakStore_.compute (source.data["path"].get<std::string>(), offset + t0 * speed, offset + t1 * speed, bins);

        if (! peaks.ok())
            return replyError (res, peaks.error());

        const auto& p = peaks.value();
        res.set_header ("X-Rovox-Bins", std::to_string (bins));
        res.set_header ("X-Rovox-T0", std::to_string (t0));
        res.set_header ("X-Rovox-T1", std::to_string (t1));
        res.set_header ("X-Rovox-Channels", std::to_string (p.channels));
        res.set_header ("Cache-Control", "no-cache");
        res.set_content (std::string ((const char*) p.data.data(), p.data.size()), "application/octet-stream");
    });

    svr.Get ("/healthz", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, 200, { { "status", "ok" }, { "version", rovoxVersion() }, { "tracktion", tracktionVersion() },
                           { "uptime_s", std::chrono::duration<double> (std::chrono::steady_clock::now() - started_).count() },
                           { "audio", toString (host_.audioMode()) }, { "cpus", host_.cpuBudget().cpus } });
    });

    // readiness for a load balancer: 200 once serving, 503 as soon as the drain begins (no authentication, like /healthz)
    svr.Get ("/readyz", [this] (const httplib::Request&, httplib::Response& res)
    {
        const bool draining = draining_.load();
        reply (res, draining ? 503 : 200, { { "ready", ! draining }, { "draining", draining } });
    });

    // Prometheus text exposition 0.0.4; the admin scope only (open mode and the operator are `local`, which has it). No key ids, no secrets.
    svr.Get ("/metrics", [this, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        auto principal = principalFor (req, res);

        if (! principal)
            return;

        if (! principal->has (Scope::admin))
            return replyError (res, forbiddenScopeError (Scope::admin, "metrics"));

        std::string out;
        const auto metric = [&out] (const std::string& name, const char* type, const char* help)
        {
            out += "# HELP " + name + " " + help + "\n# TYPE " + name + " " + type + "\n";
        };
        const auto labelEscape = [] (const std::string& v)
        {
            std::string e;

            for (char c : v)
            {
                if (c == '\\' || c == '"')  e += std::string ("\\") + c;
                else if (c == '\n')         e += "\\n";
                else                        e += c;
            }

            return e;
        };

        metric ("rovox_draining", "gauge", "1 while the server is draining for a stop, else 0");
        out += std::string ("rovox_draining ") + (draining_.load() ? "1" : "0") + "\n";

        metric ("rovox_uptime_seconds", "gauge", "Seconds since the HTTP server started");
        out += "rovox_uptime_seconds " + std::to_string (std::chrono::duration<double> (std::chrono::steady_clock::now() - started_).count()) + "\n";

        if (options_.metrics.jobsByState)
        {
            metric ("rovox_jobs", "gauge", "Jobs known to the server by state");

            for (auto& [state, n] : options_.metrics.jobsByState())
                out += "rovox_jobs{state=\"" + labelEscape (state) + "\"} " + std::to_string (n) + "\n";
        }

        if (options_.metrics.mcpSessions)
        {
            metric ("rovox_mcp_sessions", "gauge", "Open MCP sessions");
            out += "rovox_mcp_sessions " + std::to_string (options_.metrics.mcpSessions()) + "\n";
        }

        if (options_.metrics.wsClients)
        {
            metric ("rovox_ws_clients", "gauge", "Connected WebSocket clients");
            out += "rovox_ws_clients " + std::to_string (options_.metrics.wsClients()) + "\n";
        }

        if (options_.metrics.tenants)
        {
            metric ("rovox_tenant_cpu_seconds_today", "gauge", "CPU seconds used by the tenant's jobs in the current UTC day (approximate for in-process renders)");

            for (const auto& tenant : options_.metrics.tenants())
            {
                const auto used = quotas().summary (tenant).value ("used", json::object());
                out += "rovox_tenant_cpu_seconds_today{tenant=\"" + labelEscape (tenant) + "\"} " + std::to_string (used.value ("cpu_seconds_today", 0.0)) + "\n";
            }
        }

        res.status = 200;
        res.set_header ("Cache-Control", "no-store");
        res.set_content (out, "text/plain; version=0.0.4; charset=utf-8");
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
        const int status = result.ok ? 200 : (result.error ? statusFor (result.error->code) : 500);

        if (status == 429 && result.error)
            setRetryAfter (res, *result.error);

        reply (res, status, result.toJson());
    };

    // the origin comes from the route (REST = an agent over HTTP), never from the body: a client must not pass its edits off as the UI's
    // the session (the undo-coalescing key) comes from the credential plus an optional short X-Rovox-Client name, so two REST clients do not share a gesture
    auto contextFrom = [] (const httplib::Request& req, const Principal& principal, const json& body, const std::string& project)
    {
        CommandContext ctx;
        ctx.projectId = project;
        std::string session = principal.keyId.empty() ? "http:anon" : "http:" + principal.keyId;

        auto client = req.get_header_value ("X-Rovox-Client");
        const bool valid = ! client.empty() && client.size() <= 32 && std::all_of (client.begin(), client.end(), [] (unsigned char c) { return std::isalnum (c) || c == '-' || c == '_' || c == '.'; });

        if (valid)
            session += ":" + client;

        ctx.origin = { Origin::Kind::mcp, session, "http" };
        ctx.setPrincipal (principal);

        if (body.contains ("if_revision") && body["if_revision"].is_number_unsigned())
            ctx.ifRevision = body["if_revision"].get<std::uint64_t>();

        ctx.undoLabel = body.value ("undo_label", "");
        ctx.dryRun = body.value ("dry_run", false);
        return ctx;
    };

    svr.Get ("/api/projects", [runCommand, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        CommandContext ctx;
        ctx.setPrincipal (*principal);
        runCommand ("project.list", json::object(), ctx, res);
    });

    svr.Post ("/api/projects", [runCommand, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        const auto body = json::parse (req.body.empty() ? "{}" : req.body, nullptr, false);

        if (body.is_discarded())
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "body is not JSON" } } } });

        CommandContext ctx;
        ctx.setPrincipal (*principal);
        runCommand ("project.new", body, ctx, res);
    });

    svr.Get (R"(/api/projects/([^/]+))", [runCommand, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        CommandContext ctx;
        ctx.setPrincipal (*principal);
        ctx.projectId = req.matches[1];
        runCommand ("project.info", json::object(), ctx, res);
    });

    svr.Post (R"(/api/projects/([^/]+)/commands)", [runCommand, contextFrom, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        const auto body = json::parse (req.body, nullptr, false);

        if (body.is_discarded() || ! body.is_object() || ! body.contains ("name") || ! body["name"].is_string())
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "body must be {\"name\": ..., \"params\": {...}}" } } } });

        runCommand (body["name"].get<std::string>(), body.value ("params", json::object()), contextFrom (req, *principal, body, req.matches[1]), res);
    });

    for (const char* dirName : { "renders", "previews" })
    {
        const std::string dir = dirName;
        svr.Get ("/api/projects/([^/]+)/" + dir + "/([^/]+)", [this, dir, principalFor] (const httplib::Request& req, httplib::Response& res)
        {
            const auto principal = principalFor (req, res);

            if (! principal)
                return;

            const auto name = safeFileName (req.matches[2]);
            std::optional<fs::path> projectDir;      // the mutex-guarded snapshot: this runs on an HTTP thread, not the message thread

            for (auto& summary : projects_.list (principal->tenant, true))
                if (summary.id == req.matches[1])
                    projectDir = summary.dir;

            if (! projectDir || name.empty() || name[0] == '.')
                return reply (res, 404, { { "ok", false }, { "error", { { "code", errc::notFound }, { "message", "no such file" } } } });

            const auto file = *projectDir / dir / name;

            // a render that only exists in the bucket (this server started with an empty cache) is fetched on first use
            if (dir == "renders" && projects_.storage() != nullptr)
                if (std::error_code probe; ! fs::exists (file, probe))
                    projects_.storage()->fetchRender (principal->tenant, req.matches[1], name, file);

            std::ifstream in (file, std::ios::binary);

            if (! in)
                return reply (res, 404, { { "ok", false }, { "error", { { "code", errc::notFound }, { "message", "no such file" } } } });

            const auto size = fs::file_size (file);
            const auto ext = file.extension().string();
            const std::string mime = ext == ".mp3" ? "audio/mpeg" : (ext == ".flac" ? "audio/flac" : (ext == ".wav" ? "audio/wav" : "application/octet-stream"));
            auto stream = std::make_shared<std::ifstream> (std::move (in));
            res.set_content_provider (size, mime, [stream] (size_t offset, size_t length, httplib::DataSink& sink)
            {
                stream->clear();
                stream->seekg ((std::streamoff) offset);
                std::string buf (std::min<size_t> (length, 1 << 20), '\0');
                stream->read (buf.data(), (std::streamsize) buf.size());

                if (stream->gcount() <= 0)
                    return false;       // the file got shorter while it was served: close instead of looping forever

                return sink.write (buf.data(), (size_t) stream->gcount());
            });
        });
    }

    // signed download links of job files (E8-06): public, the signature (tenant + job + name + expiry) is the credential; see JobFiles.h
    auto jobFiles = std::make_shared<JobFiles> (host_.dataDir());
    svr.Get (R"(/files/([^/]+)/([^/]+)/([^/]+))", [jobFiles] (const httplib::Request& req, httplib::Response& res)
    {
        const auto now = (std::int64_t) std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count();
        const auto resolved = jobFiles->resolve (req.matches[1], req.matches[2], req.matches[3], req.get_param_value ("exp"), req.get_param_value ("sig"), now);

        if (! resolved.ok())
            return reply (res, resolved.error().code == errc::notFound ? 404 : 403, { { "ok", false }, { "error", resolved.error().toJson() } });

        const auto file = resolved.value();
        std::ifstream in (file, std::ios::binary);

        if (! in)
            return reply (res, 404, { { "ok", false }, { "error", { { "code", errc::notFound }, { "message", "no such file" } } } });

        std::error_code ec;
        const auto size = fs::file_size (file, ec);
        const auto ext = file.extension().string();
        const std::string mime = ext == ".mp3" ? "audio/mpeg" : (ext == ".flac" ? "audio/flac" : (ext == ".wav" ? "audio/wav" : "application/octet-stream"));
        res.set_header ("Content-Disposition", "attachment; filename=\"" + file.filename().string() + "\"");     // the name is [A-Za-z0-9._-]: nothing to escape
        res.set_header ("X-Content-Type-Options", "nosniff");
        res.set_header ("Cache-Control", "private, no-store");
        auto stream = std::make_shared<std::ifstream> (std::move (in));
        res.set_content_provider (size, mime, [stream] (size_t offset, size_t length, httplib::DataSink& sink)
        {
            stream->clear();
            stream->seekg ((std::streamoff) offset);
            std::string buf (std::min<size_t> (length, 1 << 20), '\0');
            stream->read (buf.data(), (std::streamsize) buf.size());

            if (stream->gcount() <= 0)
                return false;

            return sink.write (buf.data(), (size_t) stream->gcount());
        });
    });

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

    svr.Post ("/api/uploads", [this, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        tusHeaders (res);
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        if (! req.has_header ("Upload-Length"))
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "Upload-Length is required" } } } });

        std::uint64_t length = 0;

        try { length = std::stoull (req.get_header_value ("Upload-Length")); }
        catch (...) { return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "bad Upload-Length" } } } }); }

        // the tenant's storage quota is checked before the first byte is accepted (the upload is recorded under this tenant)
        if (auto quota = quotas().checkStorage (principal->tenant, (std::int64_t) std::min<std::uint64_t> (length, (std::uint64_t) std::numeric_limits<std::int64_t>::max()), principal->quotaProfile); ! quota.ok())
            return replyError (res, quota.error());

        auto created = uploads_->create (length, parseTusMetadata (req.get_header_value ("Upload-Metadata")), principal->tenant);

        if (! created.ok())
            return replyError (res, created.error());

        res.set_header ("Location", "/api/uploads/" + created.value().id);
        res.set_header ("Upload-Offset", "0");
        res.status = 201;
    });

    svr.Get (R"(/api/uploads/([^/]+))", [this, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        tusHeaders (res);
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        auto u = uploads_->info (req.matches[1], principal->tenant);

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

    svr.Delete (R"(/api/uploads/([^/]+))", [this, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        tusHeaders (res);
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        auto r = uploads_->terminate (req.matches[1], principal->tenant);
        res.status = r.ok() ? 204 : 404;
    });

    svr.Patch (R"(/api/uploads/([^/]+))", [this, principalFor] (const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader)
    {
        tusHeaders (res);
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        if (req.get_header_value ("Content-Type") != "application/offset+octet-stream")
        {
            res.status = 415;
            return;
        }

        std::uint64_t offset = 0;

        try { offset = std::stoull (req.get_header_value ("Upload-Offset")); }
        catch (...) { res.status = 400; return; }

        auto writer = uploads_->openWriter (req.matches[1], offset, principal->tenant);

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

            if (writer.value()->overflowed())
            {
                res.set_header ("Connection", "close");     // the rest of the body is not read
                res.status = 413;
                return;
            }

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

    svr.Post ("/api/upload-batches", [this, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        const auto body = json::parse (req.body, nullptr, false);

        if (body.is_discarded() || ! body.contains ("files"))
            return reply (res, 400, { { "ok", false }, { "error", { { "code", errc::invalidParams }, { "message", "body must be {\"files\": [{\"relpath\", \"size\"}]}" } } } });

        auto batch = uploads_->createBatch (body["files"], principal->tenant);
        batch.ok() ? reply (res, 201, batch.value()) : replyError (res, batch.error());
    });

    svr.Get (R"(/api/upload-batches/([^/]+))", [this, principalFor] (const httplib::Request& req, httplib::Response& res)
    {
        const auto principal = principalFor (req, res);

        if (! principal)
            return;

        auto status = uploads_->batchStatus (req.matches[1], principal->tenant);
        status.ok() ? reply (res, 200, status.value()) : replyError (res, status.error());
    });
}

} // namespace rovox
