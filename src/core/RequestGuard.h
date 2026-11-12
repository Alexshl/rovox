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

inline std::string lowered (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

/** localhost, the whole 127.0.0.0/8 and ::1 (host names compare case-insensitively). */
inline bool isLoopbackName (const std::string& hostName)
{
    const auto host = lowered (hostName);

    if (host == "localhost" || host == "[::1]" || host == "::1")
        return true;

    if (host.rfind ("127.", 0) != 0)
        return false;

    int dots = 0;

    for (const char c : host)
    {
        if (c == '.')
            ++dots;
        else if (c < '0' || c > '9')
            return false;
    }

    return dots == 3 && host.back() != '.';
}

inline bool isLoopbackBind (const std::string& bind)
{
    return isLoopbackName (bind);
}

/** DNS rebinding defence: when bound to loopback, the Host header must name a loopback address, one of `allowedHosts`
    ("host" = any port, "host:port" = exactly that) or the host of an `allowedOrigins` entry (a reverse proxy forwarding
    `Host: daw.example.com`). A missing Host passes: HTTP/1.0 clients. */
inline bool hostAllowed (bool loopbackBind, const std::string& hostHeader, const std::vector<std::string>& allowedHosts = {},
                         const std::vector<std::string>& allowedOrigins = {})
{
    if (! loopbackBind || hostHeader.empty() || isLoopbackName (hostOf (hostHeader)))
        return true;

    const auto full = lowered (hostHeader), name = lowered (hostOf (hostHeader));

    for (const auto& entry : allowedHosts)
    {
        const auto e = lowered (entry);

        if (e == full || (e == hostOf (e) && e == name))
            return true;
    }

    for (const auto& origin : allowedOrigins)
        if (lowered (hostOf (origin)) == name)
            return true;

    return false;
}

/** Loopback origins and the explicit allow-list (full origins such as https://example.com). */
inline bool originInList (const std::string& origin, const std::vector<std::string>& allowed)
{
    return isLoopbackName (hostOf (origin)) || std::find (allowed.begin(), allowed.end(), origin) != allowed.end();
}

/** Browser requests: no Origin = not a browser (allowed); an Origin that merely equals a non-loopback Host is only the page itself
    when the server is not bound to loopback. */
inline bool requestAllowed (bool loopbackBind, const std::string& origin, const std::string& host, const std::vector<std::string>& allowed,
                            const std::vector<std::string>& allowedHosts = {})
{
    if (! hostAllowed (loopbackBind, host, allowedHosts, allowed))
        return false;

    if (origin.empty() || originInList (origin, allowed))
        return true;

    return ! loopbackBind && ! host.empty() && hostOf (origin) == hostOf (host);
}

} // namespace rovox::guard
