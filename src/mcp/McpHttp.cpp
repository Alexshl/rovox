// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "McpHttp.h"

#include "RequestGuard.h"

#include <httplib.h>

#include <atomic>
#include <mutex>
#include <set>

namespace rovox::mcp
{

namespace
{
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

/** Counts one request on the session until the returned handle (and every copy of it) is gone. */
std::shared_ptr<void> markBusy (const std::shared_ptr<Session>& session)
{
    ++session->inFlight;
    return std::shared_ptr<void> (nullptr, [session] (void*) { --session->inFlight; });
}

bool wantsProgress (const json& body)
{
    return body.value ("method", "") == "tools/call" && body.contains ("params") && body["params"].is_object() && body["params"].contains ("_meta")
           && body["params"]["_meta"].is_object() && body["params"]["_meta"].contains ("progressToken");
}
} // namespace

bool originAllowed (const std::string& origin, const std::vector<std::string>& allowed)
{
    return guard::originInList (origin, allowed);
}

bool hostAllowed (const std::string& hostHeader, const std::vector<std::string>& allowedHosts)
{
    return guard::hostAllowed (true, hostHeader, allowedHosts);
}

void installMcpRoutes (httplib::Server& svr, McpServer& server, McpHttpOptions options)
{
    // sessions and SSE streams each cost a pool worker or memory: bounded, then 503 / 429
    struct Limits
    {
        std::mutex mutex;
        std::set<std::string> sessionIds;
        std::atomic<int> streams { 0 };
    };

    auto limits = std::make_shared<Limits>();
    const auto maxSessions = options.maxSessions, maxStreams = options.maxStreams;

    auto guard = [options] (const httplib::Request& req, httplib::Response& res) -> bool
    {
        // the same rule as REST and WebSocket (guard::requestAllowed): DNS rebinding and cross-origin pages
        if (! guard::requestAllowed (options.loopbackBind, req.get_header_value ("Origin"), req.get_header_value ("Host"), options.allowedOrigins, options.allowedHosts))
        {
            replyJson (res, 403, rpcError (nullptr, -32000, "Origin or Host not allowed"));
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

    svr.Post ("/mcp", [&server, guard, limits, maxSessions, maxStreams] (const httplib::Request& req, httplib::Response& res)
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
        std::shared_ptr<void> busy;         // keeps the session out of LRU eviction while this request (or its streaming worker) runs

        if (isInit)
        {
            {
                std::lock_guard<std::mutex> lock (limits->mutex);

                for (auto it = limits->sessionIds.begin(); it != limits->sessionIds.end();)       // forget sessions that expired or were closed
                    it = server.findSession (*it) ? std::next (it) : limits->sessionIds.erase (it);

                if (limits->sessionIds.size() >= maxSessions)
                {
                    // make room: close the least recently used session that has no request in flight and no open stream
                    std::shared_ptr<Session> victim;

                    for (const auto& sid : limits->sessionIds)
                        if (auto s = server.findSession (sid); s && s->inFlight.load() == 0 && s->openStreams.load() == 0
                                                               && (! victim || s->lastActive() < victim->lastActive()))
                            victim = s;

                    if (! victim)
                    {
                        res.set_header ("Retry-After", "30");
                        return replyJson (res, 503, rpcError (nullptr, -32000, "too many MCP sessions"));
                    }

                    server.closeSession (victim->id);
                    limits->sessionIds.erase (victim->id);
                }

                session = server.createSession();
                limits->sessionIds.insert (session->id);
                busy = markBusy (session);
            }

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

            busy = markBusy (session);
        }

        session->touch();
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
            if (limits->streams.fetch_add (1) >= (int) maxStreams)
            {
                --limits->streams;
                return replyJson (res, 429, rpcError (body.value ("id", json (nullptr)), -32000, "too many concurrent event streams"));
            }

            auto channel = std::make_shared<Channel>();
            rc.emit = [channel] (const json& n) { channel->push (n, false); };
            channel->worker = std::thread ([channel, &server, session, body, rc, busy]() mutable
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
            [channel, limits] (bool)
            {
                --limits->streams;

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

    svr.Get ("/mcp", [&server, guard, limits, maxStreams] (const httplib::Request& req, httplib::Response& res)
    {
        if (! guard (req, res))
            return;

        if (req.get_header_value ("Accept").find ("text/event-stream") == std::string::npos)
            return replyJson (res, 406, rpcError (nullptr, -32000, "Accept must include text/event-stream"));

        auto session = server.findSession (req.get_header_value ("Mcp-Session-Id"));

        if (! session)
            return replyJson (res, 404, rpcError (nullptr, -32001, "unknown or expired session; initialize again"));

        if (limits->streams.fetch_add (1) >= (int) maxStreams)
        {
            --limits->streams;
            return replyJson (res, 429, rpcError (nullptr, -32000, "too many concurrent event streams"));
        }

        session->touch();
        ++session->openStreams;
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
        },
        [limits, session] (bool)
        {
            --limits->streams;
            --session->openStreams;
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
