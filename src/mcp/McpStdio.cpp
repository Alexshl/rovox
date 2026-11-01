// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "McpStdio.h"

#include <iostream>

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

    // workers for requests that wait on jobs
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<std::function<void()>> queue;
    bool closing = false;
    std::vector<std::thread> workers;

    for (int i = 0; i < 8; ++i)
        workers.emplace_back ([&]
        {
            for (;;)
            {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock (queueMutex);
                    queueCv.wait (lock, [&] { return closing || ! queue.empty(); });

                    if (queue.empty())
                        return;

                    task = std::move (queue.front());
                    queue.pop_front();
                }

                task();
            }
        });

    auto process = [this, session] (const json& msg)
    {
        RequestContext rc;
        rc.emit = [this] (const json& n) { write (n); };

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

        if (McpServer::isRequest (msg) && server_.mayBlock (msg))
        {
            {
                std::lock_guard<std::mutex> lock (queueMutex);
                queue.push_back ([process, msg] { process (msg); });
            }

            queueCv.notify_one();
        }
        else
        {
            process (msg);
        }
    }

    // input closed: let running calls finish (a job wait ends at its own deadline)
    {
        std::lock_guard<std::mutex> lock (queueMutex);
        closing = true;
    }

    queueCv.notify_all();

    for (auto& w : workers)
        w.join();

    done = true;
    pump.join();
    server_.closeSession (session->id);
}

} // namespace rovox::mcp
