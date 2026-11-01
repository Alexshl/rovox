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
    bool loopbackBind = true;                   // then the Host header must be a loopback name too (rebinding defence)
};

void installMcpRoutes (httplib::Server&, McpServer&, McpHttpOptions);

/** Exposed for tests. */
bool originAllowed (const std::string& origin, const std::vector<std::string>& allowed);
bool hostAllowed (const std::string& hostHeader);

} // namespace rovox::mcp
