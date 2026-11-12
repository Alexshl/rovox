// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "McpStdio.h"

#include <iostream>
#include <map>

namespace rovox::mcp
{

StdioTransport::StdioTransport (McpServer& s, std::istream& in, std::ostream& out) : server_ (s), in_ (in), out_ (out) {}

void StdioTransport::write (const json& j)
{
    const auto line = j.dump (-1, ' ', false, json::error_handler_t::replace);
    std::lock_guard<std::mutex> lock (writeMutex_);
    out_ << line << "\n" << std::flush;
}

void StdioTransport::run()
{
    auto session = server_.createSession();
    std::atomic<bool> done { false };

    // server-initiated notifications (events, resource updates) of this session
    std::thread pump ([&]
    {
        std::uint64_t cursor = session->lastMessageId();
        json m;

        while (! done)
            if (session->next (cursor, m, std::chrono::milliseconds (200), &done))
                write (m);
    });

    // Two lanes besides the reader: eight workers for requests that wait on jobs, and one ordered lane for the other tool calls
    // (they keep their arrival order but never hold up the reader, so cancel and ping are processed while one runs).
    struct Lane
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::function<void()>> queue;
        bool closing = false;
        std::vector<std::thread> threads;

        void start (int n)
        {
            for (int i = 0; i < n; ++i)
                threads.emplace_back ([this]
                {
                    for (;;)
                    {
                        std::function<void()> task;

                        {
                            std::unique_lock<std::mutex> lock (mutex);
                            cv.wait (lock, [&] { return closing || ! queue.empty(); });

                            if (queue.empty())
                                return;

                            task = std::move (queue.front());
                            queue.pop_front();
                        }

                        task();
                    }
                });
        }

        void push (std::function<void()> f)
        {
            {
                std::lock_guard<std::mutex> lock (mutex);
                queue.push_back (std::move (f));
            }

            cv.notify_one();
        }

        void finish()
        {
            {
                std::lock_guard<std::mutex> lock (mutex);
                closing = true;
            }

            cv.notify_all();

            for (auto& t : threads)
                t.join();
        }
    };

    Lane blocking, ordered;
    blocking.start (8);
    ordered.start (1);

    // cancel flags of requests that were handed to a lane: a cancel can arrive while the request is still queued
    std::mutex pendingMutex;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> pendingCancel;

    auto process = [this, session] (const json& msg, std::shared_ptr<std::atomic<bool>> cancelled)
    {
        RequestContext rc;
        rc.emit = [this] (const json& n) { write (n); };

        if (cancelled != nullptr)
            rc.cancelled = cancelled;

        if (auto response = server_.handle (*session, msg, rc))
            write (*response);
    };

    std::string line;

    while (std::getline (in_, line))
    {
        if (line.find_first_not_of (" \t\r\n") == std::string::npos)
            continue;

        auto msg = json::parse (line, nullptr, false);

        if (msg.is_discarded())
        {
            write (rpcError (nullptr, -32700, "parse error"));
            continue;
        }

        if (msg.is_array())
        {
            write (rpcError (nullptr, -32600, "batching is not supported in MCP 2025-06-18"));
            continue;
        }

        if (McpServer::isNotification (msg) && msg.value ("method", "") == "notifications/cancelled"
            && msg.contains ("params") && msg["params"].is_object() && msg["params"].contains ("requestId"))
        {
            std::lock_guard<std::mutex> lock (pendingMutex);

            if (auto it = pendingCancel.find (msg["params"]["requestId"].dump()); it != pendingCancel.end())
                it->second->store (true);
        }

        const bool isToolCall = McpServer::isRequest (msg) && msg.value ("method", "") == "tools/call";

        if (isToolCall)
        {
            const auto key = msg["id"].dump();
            auto cancelled = std::make_shared<std::atomic<bool>> (false);

            {
                std::lock_guard<std::mutex> lock (pendingMutex);
                pendingCancel[key] = cancelled;
            }

            auto task = [&process, &pendingMutex, &pendingCancel, msg, key, cancelled]
            {
                process (msg, cancelled);
                std::lock_guard<std::mutex> lock (pendingMutex);
                pendingCancel.erase (key);
            };

            (server_.mayBlock (msg) ? blocking : ordered).push (std::move (task));
        }
        else
        {
            process (msg, nullptr);
        }
    }

    // input closed: let running calls finish (a job wait ends at its own deadline)
    ordered.finish();
    blocking.finish();

    done = true;
    pump.join();
    server_.closeSession (session->id);
}

} // namespace rovox::mcp
