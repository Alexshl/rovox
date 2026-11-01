// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "McpHttp.h"

#include <httplib.h>

namespace rovox::mcp
{

namespace
{
std::string hostOf (std::string s)
{
    if (const auto scheme = s.find ("://"); scheme != std::string::npos)
        s = s.substr (scheme + 3);

    if (const auto slash = s.find ('/'); slash != std::string::npos)
        s = s.substr (0, slash);

    if (! s.empty() && s[0] == '[')
        return s.substr (0, s.find (']') + 1);

    if (const auto colon = s.find (':'); colon != std::string::npos)
        s = s.substr (0, colon);

    return s;
}

bool isLoopbackName (const std::string& host)
{
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

std::string dumpSafe (const json& j)    { return j.dump (-1, ' ', false, json::error_handler_t::replace); }

void replyJson (httplib::Response& res, int status, const json& body)
{
    res.status = status;
    res.set_content (dumpSafe (body), "application/json");
}

std::string sseEvent (const json& m, std::optional<std::uint64_t> id)
{
    return (id ? "id: " + std::to_string (*id) + "\n" : std::string()) + "event: message\ndata: " + dumpSafe (m) + "\n\n";
}

/** Carries the messages of one POST that answers with an event stream (progress notifications, then the response). */
struct Channel
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<json> messages;
    bool finished = false;
    std::thread worker;

    void push (json m, bool last)
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            messages.push_back (std::move (m));
            finished = finished || last;
        }

        cv.notify_all();
    }
};

bool wantsProgress (const json& body)
{
    return body.value ("method", "") == "tools/call" && body.contains ("params") && body["params"].is_object() && body["params"].contains ("_meta")
           && body["params"]["_meta"].is_object() && body["params"]["_meta"].contains ("progressToken");
}
} // namespace

bool originAllowed (const std::string& origin, const std::vector<std::string>& allowed)
{
    if (isLoopbackName (hostOf (origin)))
        return true;

    for (auto& a : allowed)
        if (a == origin)
            return true;

    return false;
}

bool hostAllowed (const std::string& hostHeader)
{
    return hostHeader.empty() || isLoopbackName (hostOf (hostHeader));
}

void installMcpRoutes (httplib::Server& svr, McpServer& server, McpHttpOptions options)
{
    auto guard = [options] (const httplib::Request& req, httplib::Response& res) -> bool
    {
        // DNS rebinding: a browser page on another origin must not be able to talk to a local server
        if (req.has_header ("Origin") && ! originAllowed (req.get_header_value ("Origin"), options.allowedOrigins))
        {
            replyJson (res, 403, rpcError (nullptr, -32000, "Origin not allowed"));
            return false;
        }

        if (options.loopbackBind && ! hostAllowed (req.get_header_value ("Host")))
        {
            replyJson (res, 403, rpcError (nullptr, -32000, "Host not allowed"));
            return false;
        }

        const auto version = req.get_header_value ("MCP-Protocol-Version");

        if (! version.empty() && version != "2025-06-18" && version != "2025-03-26")
        {
            replyJson (res, 400, rpcError (nullptr, -32000, "unsupported MCP-Protocol-Version " + version));
            return false;
        }

        return true;
    };

    svr.Post ("/mcp", [&server, guard] (const httplib::Request& req, httplib::Response& res)
    {
        if (! guard (req, res))
            return;

        auto body = json::parse (req.body, nullptr, false);

        if (body.is_discarded())
            return replyJson (res, 400, rpcError (nullptr, -32700, "parse error"));

        if (! body.is_object())
            return replyJson (res, 400, rpcError (nullptr, -32600, "batching is not supported in MCP 2025-06-18; send one message per POST"));

        const bool isInit = body.value ("method", "") == "initialize";
        std::shared_ptr<Session> session;

        if (isInit)
        {
            session = server.createSession();
            res.set_header ("Mcp-Session-Id", session->id);
        }
        else
        {
            const auto id = req.get_header_value ("Mcp-Session-Id");

            if (id.empty())
                return replyJson (res, 400, rpcError (body.value ("id", json (nullptr)), -32000, "Mcp-Session-Id header is required after initialize"));

            session = server.findSession (id);

            if (! session)
                return replyJson (res, 404, rpcError (body.value ("id", json (nullptr)), -32001, "unknown or expired session; initialize again"));
        }

        RequestContext rc;
        Session* raw = session.get();
        rc.emit = [raw] (const json& n) { raw->push (n); };

        if (! McpServer::isRequest (body))
        {
            server.handle (*session, body, rc);
            res.status = 202;
            return;
        }

        if (wantsProgress (body) && ! isInit)
        {
            auto channel = std::make_shared<Channel>();
            rc.emit = [channel] (const json& n) { channel->push (n, false); };
            channel->worker = std::thread ([channel, &server, session, body, rc]() mutable
            {
                auto response = server.handle (*session, body, rc);
                channel->push (response ? *response : json (nullptr), true);
            });

            res.set_header ("Cache-Control", "no-cache");
            res.set_chunked_content_provider ("text/event-stream", [channel, &server] (size_t, httplib::DataSink& sink)
            {
                std::unique_lock<std::mutex> lock (channel->mutex);
                channel->cv.wait_for (lock, std::chrono::seconds (15), [&] { return ! channel->messages.empty(); });

                if (channel->messages.empty())
                {
                    lock.unlock();
                    return sink.write (": keepalive\n\n", 13);
                }

                std::string out;

                while (! channel->messages.empty())
                {
                    if (! channel->messages.front().is_null())
                        out += sseEvent (channel->messages.front(), std::nullopt);

                    channel->messages.pop_front();
                }

                const bool finished = channel->finished;
                lock.unlock();

                if (! out.empty() && ! sink.write (out.data(), out.size()))
                    return false;

                if (finished)
                    sink.done();

                (void) server;
                return true;
            },
            [channel] (bool)
            {
                if (channel->worker.joinable())
                    channel->worker.join();
            });
            return;
        }

        auto response = server.handle (*session, body, rc);

        if (! response)
        {
            res.status = 202;
            return;
        }

        replyJson (res, 200, *response);
    });

    svr.Get ("/mcp", [&server, guard] (const httplib::Request& req, httplib::Response& res)
    {
        if (! guard (req, res))
            return;

        if (req.get_header_value ("Accept").find ("text/event-stream") == std::string::npos)
            return replyJson (res, 406, rpcError (nullptr, -32000, "Accept must include text/event-stream"));

        auto session = server.findSession (req.get_header_value ("Mcp-Session-Id"));

        if (! session)
            return replyJson (res, 404, rpcError (nullptr, -32001, "unknown or expired session; initialize again"));

        session->touch();
        const int generation = ++session->streamGeneration;
        std::uint64_t cursor = session->lastMessageId();

        // resume after a dropped stream: replay what the client has not seen
        if (const auto last = req.get_header_value ("Last-Event-ID"); ! last.empty())
        {
            try { cursor = std::stoull (last); }
            catch (...) {}
        }

        res.set_header ("Cache-Control", "no-cache");
        auto idleSlices = std::make_shared<int> (0);
        res.set_chunked_content_provider ("text/event-stream", [&server, session, generation, cursor, idleSlices] (size_t, httplib::DataSink& sink) mutable
        {
            if (session->streamGeneration.load() != generation || server.stoppingFlag().load())
            {
                sink.done();
                return true;
            }

            json m;

            if (session->next (cursor, m, std::chrono::milliseconds (1000), &server.stoppingFlag()))
            {
                *idleSlices = 0;
                const auto text = sseEvent (m, cursor);
                session->touch();
                return sink.write (text.data(), text.size());
            }

            if (++*idleSlices >= 15)
            {
                *idleSlices = 0;
                session->touch();
                return sink.write (": keepalive\n\n", 13);
            }

            return true;
        });
    });

    svr.Delete ("/mcp", [&server, guard] (const httplib::Request& req, httplib::Response& res)
    {
        if (! guard (req, res))
            return;

        auto session = server.findSession (req.get_header_value ("Mcp-Session-Id"));

        if (! session)
            return replyJson (res, 404, rpcError (nullptr, -32001, "unknown or expired session"));

        server.closeSession (session->id);
        res.status = 204;
    });
}

} // namespace rovox::mcp
