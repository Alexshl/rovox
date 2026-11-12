// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// MCP Streamable HTTP (task 200, spec 2025-06-18): POST /mcp (request -> JSON or SSE), GET /mcp (server notifications as SSE),
// DELETE /mcp (close the session). Origin is checked against DNS rebinding; the bearer token, when configured, is checked by
// the HTTP server's pre-routing handler for every path.
#pragma once

#include "Mcp.h"

namespace httplib { class Server; }

namespace rovox::mcp
{

struct McpHttpOptions
{
    std::vector<std::string> allowedOrigins;    // full origins such as https://example.com, besides loopback
    std::vector<std::string> allowedHosts;      // extra Host values accepted on a loopback bind (reverse proxy)
    bool loopbackBind = true;                   // the same Origin / Host rule as REST and WebSocket (rebinding defence; same-host Origin only off loopback)
    size_t maxSessions = 256;                   // initialize beyond this answers 503
    size_t maxStreams = 32;                     // concurrent SSE streams (each holds a pool worker); beyond this 429
};

void installMcpRoutes (httplib::Server&, McpServer&, McpHttpOptions);

/** Exposed for tests. */
bool originAllowed (const std::string& origin, const std::vector<std::string>& allowed);
bool hostAllowed (const std::string& hostHeader, const std::vector<std::string>& allowedHosts = {});

} // namespace rovox::mcp
