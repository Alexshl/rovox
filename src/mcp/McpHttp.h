// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// MCP Streamable HTTP (task 200, spec 2025-06-18): POST /mcp (request -> JSON or SSE), GET /mcp (server notifications as SSE),
// DELETE /mcp (close the session). Origin is checked against DNS rebinding; the credentials are checked by the HTTP server's
// pre-routing handler for every path, and again here to bind a session to the key that created it (another key presenting
// the Mcp-Session-Id gets "unknown session") and to rate limit each POST.
#pragma once

#include "Auth.h"
#include "Mcp.h"

namespace httplib { class Server; }

namespace rovox::mcp
{

struct McpHttpOptions
{
    Authenticator* authenticator = nullptr;     // not owned; null = no per-request identity: every session is the local operator's (the HTTP server's pre-routing may still require a token)
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
