// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Minimal HTTP surface of `rovox serve` (tasks 100/140): /healthz, the command layer over HTTP, project and render
// downloads, and the tus 1.0 upload endpoints. MCP and WebSocket arrive in M2/M3; they will use the same CommandBus.
#pragma once

#include "CommandBus.h"
#include "ProjectManager.h"
#include "UploadStore.h"

#include <memory>
#include <thread>

namespace httplib { class Server; }

namespace rovox
{

struct ServerOptions
{
    std::string bind = "127.0.0.1";
    int port = 4810;                    // 0 = pick a free port (tests)
    std::string token;                  // empty = no authentication (loopback default); otherwise `Authorization: Bearer <token>`
    UploadLimits uploadLimits;
};

class HttpServer
{
public:
    HttpServer (EngineHost&, ProjectManager&, CommandBus&, EventHub&, ServerOptions);
    ~HttpServer();

    /** Binds and starts serving on a background thread. Fails (port in use, bad address) before returning. */
    Result<void> start();
    void stop();
    int port() const { return port_; }
    UploadStore& uploads() { return *uploads_; }

private:
    void installRoutes();

    EngineHost& host_;
    ProjectManager& projects_;
    CommandBus& bus_;
    EventHub& hub_;
    ServerOptions options_;
    std::unique_ptr<UploadStore> uploads_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    int port_ = 0;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

} // namespace rovox
