// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Url.h"

#include <algorithm>
#include <cctype>

namespace rovox
{

namespace
{
Url fail (Url u, const std::string& why)
{
    u.valid = false;
    u.error = why;
    return u;
}
} // namespace

Url parseUrl (const std::string& url)
{
    Url u;
    const auto schemeEnd = url.find ("://");

    if (schemeEnd == std::string::npos)
        return fail (u, "the URL has no scheme");

    u.scheme = url.substr (0, schemeEnd);
    std::transform (u.scheme.begin(), u.scheme.end(), u.scheme.begin(), [] (unsigned char c) { return (char) std::tolower (c); });

    if (u.scheme != "http" && u.scheme != "https")
        return fail (u, "the URL scheme must be http or https");

    const auto hostStart = schemeEnd + 3;
    const auto authorityEnd = url.find_first_of ("/?#", hostStart);
    const auto authority = url.substr (hostStart, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - hostStart);

    if (authorityEnd != std::string::npos && url[authorityEnd] != '#')
    {
        auto rest = url.substr (authorityEnd);

        if (const auto hash = rest.find ('#'); hash != std::string::npos)
            rest.resize (hash);

        u.path = rest.front() == '?' ? "/" + rest : rest;
    }

    if (authority.find ('@') != std::string::npos)
        return fail (u, "the URL must not contain user info");

    std::string portText;
    bool bracketed = false;

    if (! authority.empty() && authority.front() == '[')
    {
        const auto close = authority.find (']');

        if (close == std::string::npos)
            return fail (u, "the IPv6 address in the URL is not closed with ]");

        u.host = authority.substr (1, close - 1);
        bracketed = true;

        if (u.host.empty() || u.host.find_first_not_of ("0123456789abcdefABCDEF:.") != std::string::npos || u.host.find (':') == std::string::npos)
            return fail (u, "the IPv6 address in the URL is malformed");

        const auto tail = authority.substr (close + 1);

        if (! tail.empty())
        {
            if (tail.front() != ':')
                return fail (u, "unexpected characters after the IPv6 address in the URL");

            portText = tail.substr (1);

            if (portText.empty())
                return fail (u, "the URL port is empty");
        }
    }
    else
    {
        const auto colon = authority.find (':');
        u.host = authority.substr (0, colon);

        if (colon != std::string::npos)
        {
            portText = authority.substr (colon + 1);

            if (portText.empty() || portText.find (':') != std::string::npos)
                return fail (u, portText.empty() ? "the URL port is empty" : "an IPv6 address in a URL must be written in [brackets]");
        }
    }

    if (u.host.empty())
        return fail (u, "the URL has no host");

    u.port = u.scheme == "https" ? 443 : 80;

    if (! portText.empty())
    {
        if (portText.size() > 5 || portText.find_first_not_of ("0123456789") != std::string::npos)
            return fail (u, "the URL port is not a number");

        const int p = std::stoi (portText);

        if (p < 1 || p > 65535)
            return fail (u, "the URL port must be 1..65535");

        u.port = p;
    }

    const std::string shownHost = bracketed ? "[" + u.host + "]" : u.host;
    u.origin = u.scheme + "://" + shownHost + (portText.empty() ? "" : ":" + std::to_string (u.port));
    u.hostHeader = shownHost + ((u.scheme == "https" && u.port == 443) || (u.scheme == "http" && u.port == 80) ? "" : ":" + std::to_string (u.port));
    u.valid = true;
    return u;
}

} // namespace rovox
