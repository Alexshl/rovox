// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Origin / Host guard shared by REST, WebSocket and MCP: a browser page on another origin (CSRF) or on a rebound DNS name
// must not be able to talk to a server that listens on loopback.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace rovox::guard
{

/** "https://host:port/path", "host:port" or "[::1]:port" -> "host" / "[::1]". */
inline std::string hostOf (std::string s)
{
    if (const auto scheme = s.find ("://"); scheme != std::string::npos)
        s = s.substr (scheme + 3);

    if (const auto slash = s.find ('/'); slash != std::string::npos)
        s = s.substr (0, slash);

    if (! s.empty() && s[0] == '[')
        return s.substr (0, s.find (']') + 1);

    return s.substr (0, s.find (':'));
}

inline bool isLoopbackName (const std::string& host)
{
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]" || host == "::1";
}

inline bool isLoopbackBind (const std::string& bind)
{
    return bind == "127.0.0.1" || bind == "localhost" || bind == "::1" || bind == "[::1]";
}

/** DNS rebinding defence: when bound to loopback, the Host header must name a loopback address (a missing Host passes: HTTP/1.0 clients). */
inline bool hostAllowed (bool loopbackBind, const std::string& hostHeader)
{
    return ! loopbackBind || hostHeader.empty() || isLoopbackName (hostOf (hostHeader));
}

/** Loopback origins and the explicit allow-list (full origins such as https://example.com). */
inline bool originInList (const std::string& origin, const std::vector<std::string>& allowed)
{
    return isLoopbackName (hostOf (origin)) || std::find (allowed.begin(), allowed.end(), origin) != allowed.end();
}

/** Browser requests: no Origin = not a browser (allowed); an Origin that merely equals a non-loopback Host is only the page itself
    when the server is not bound to loopback. */
inline bool requestAllowed (bool loopbackBind, const std::string& origin, const std::string& host, const std::vector<std::string>& allowed)
{
    if (! hostAllowed (loopbackBind, host))
        return false;

    if (origin.empty() || originInList (origin, allowed))
        return true;

    return ! loopbackBind && ! host.empty() && hostOf (origin) == hostOf (host);
}

} // namespace rovox::guard
