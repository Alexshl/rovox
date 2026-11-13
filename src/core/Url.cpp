// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Url.h"

#include <httplib.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>

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

bool privateV4 (const std::uint8_t* b)
{
    return b[0] == 0 || b[0] == 10 || b[0] == 127 || (b[0] == 169 && b[1] == 254) || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168)
           || (b[0] == 100 && (b[1] & 0xc0) == 64) || (b[0] == 198 && (b[1] & 0xfe) == 18) || b[0] >= 224;
}

/** Absolute URL of a Location header against the URL it answered; empty if there is none. */
std::string redirectTarget (const Url& base, const std::string& location)
{
    if (location.empty())
        return {};

    if (location.find ("://") != std::string::npos)
        return location;

    if (location.rfind ("//", 0) == 0)
        return base.scheme + ":" + location;

    if (location.front() == '/')
        return base.origin + location;

    auto dir = base.path.substr (0, base.path.find ('?'));
    dir = dir.empty() ? "/" : dir.substr (0, dir.rfind ('/') + 1);
    return base.origin + dir + location;
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

bool isPrivateAddress (const std::string& ip)
{
    in_addr a4 {};

    if (inet_pton (AF_INET, ip.c_str(), &a4) == 1)
        return privateV4 (reinterpret_cast<const std::uint8_t*> (&a4));

    in6_addr a6 {};

    if (inet_pton (AF_INET6, ip.c_str(), &a6) != 1)
        return true;        // not an address we can judge: treat as unsafe

    const auto* b = a6.s6_addr;
    static const std::uint8_t zeros[12] = {};

    if (std::memcmp (b, zeros, 10) == 0 && b[10] == 0xff && b[11] == 0xff)          // ::ffff:a.b.c.d
        return privateV4 (b + 12);

    if (std::memcmp (b, zeros, 12) == 0)                                            // ::a.b.c.d (IPv4-compatible, deprecated), :: and ::1
        return true;

    static const std::uint8_t nat64[12] = { 0x00, 0x64, 0xff, 0x9b };

    if (std::memcmp (b, nat64, 12) == 0)                                            // 64:ff9b::/96: the embedded IPv4 address decides
        return privateV4 (b + 12);

    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b && b[4] == 0x00 && b[5] == 0x01)        // 64:ff9b:1::/48 local-use NAT64
        return true;

    if (b[0] == 0x20 && b[1] == 0x02)                                               // 6to4 2002::/16: the embedded IPv4 address decides
        return privateV4 (b + 2);

    return (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) || (b[0] & 0xfe) == 0xfc || b[0] == 0xff;
}

bool isLoopbackAddress (const std::string& ip)
{
    in_addr a4 {};

    if (inet_pton (AF_INET, ip.c_str(), &a4) == 1)
        return reinterpret_cast<const std::uint8_t*> (&a4)[0] == 127;

    in6_addr a6 {};

    if (inet_pton (AF_INET6, ip.c_str(), &a6) != 1)
        return false;

    const auto* b = a6.s6_addr;
    static const std::uint8_t zeros[15] = {};

    if (std::memcmp (b, zeros, 15) == 0)
        return b[15] == 1;                                                          // ::1

    return std::memcmp (b, zeros, 10) == 0 && b[10] == 0xff && b[11] == 0xff && b[12] == 127;      // ::ffff:127.x.x.x
}

/** Every address the host resolves to (numeric strings). */
std::vector<std::string> resolveHost (const std::string& host, int port)
{
    std::vector<std::string> out;
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;

    if (getaddrinfo (host.c_str(), std::to_string (port).c_str(), &hints, &res) != 0)
        return out;

    for (auto* a = res; a != nullptr; a = a->ai_next)
    {
        char buf[INET6_ADDRSTRLEN] = {};

        if (a->ai_family == AF_INET)
            inet_ntop (AF_INET, &reinterpret_cast<sockaddr_in*> (a->ai_addr)->sin_addr, buf, sizeof (buf));
        else if (a->ai_family == AF_INET6)
            inet_ntop (AF_INET6, &reinterpret_cast<sockaddr_in6*> (a->ai_addr)->sin6_addr, buf, sizeof (buf));
        else
            continue;

        out.push_back (buf);
    }

    freeaddrinfo (res);
    return out;
}

Result<std::vector<std::string>> checkUrlHost (const Url& url, const UrlGuard& guard)
{
    std::string host = url.host;

    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr (1, host.size() - 2);

    auto addresses = guard.resolver ? guard.resolver (host, url.port) : resolveHost (host, url.port);

    if (addresses.empty())
        return makeError (errc::invalidParams, "the host of the URL cannot be resolved");

    if (! guard.allowPrivate)
        for (auto& a : addresses)
            if (isPrivateAddress (a) && ! (guard.allowLoopback && isLoopbackAddress (a)))
                return makeError (errc::invalidParams, "the URL resolves to a loopback, private or link-local address");

    return addresses;
}

Result<FetchResult> fetchUrlToFile (const std::string& startUrl, const std::filesystem::path& dest, const FetchOptions& opts)
{
    namespace fs = std::filesystem;
    const auto started = std::chrono::steady_clock::now();
    auto fail = [&] (Error e) -> Result<FetchResult>
    {
        std::error_code ec;
        fs::remove (dest, ec);
        return e;
    };

    std::string current = startUrl;

    for (int hop = 0; hop <= opts.maxRedirects; ++hop)
    {
        const auto url = parseUrl (current);

        if (! url.valid)
            return fail (makeError (errc::invalidParams, url.error.empty() ? "the URL is not valid" : url.error));

        auto addresses = checkUrlHost (url, opts.guard);

        if (! addresses.ok())
            return fail (addresses.error());

        httplib::Client client (url.origin);
        client.set_hostname_addr_map ({ { url.host, addresses.value().front() } });      // connect to the address that was checked, not to a second DNS answer
        client.set_follow_location (false);                                            // every redirect goes through the guard here
        client.set_connection_timeout (opts.connectTimeoutS, 0);
        client.set_read_timeout (opts.readTimeoutS, 0);
        client.set_write_timeout (opts.readTimeoutS, 0);

        std::ofstream sink (dest, std::ios::binary | std::ios::trunc);

        if (! sink)
            return fail (makeError (errc::io, "cannot write the download file"));

        std::optional<Error> refused;
        std::string redirectTo, contentType;
        std::int64_t total = -1;
        std::uint64_t received = 0;
        bool accepted = false;

        auto result = client.Get (url.path.empty() ? "/" : url.path, httplib::Headers { { "Accept", "*/*" } },
                                  [&] (const httplib::Response& r)
                                  {
                                      if (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308)
                                      {
                                          redirectTo = r.get_header_value ("Location");
                                          return false;
                                      }

                                      if (r.status != 200)
                                      {
                                          refused = makeError (errc::preconditionFailed, "the server answered HTTP " + std::to_string (r.status), json { { "http_status", r.status } });
                                          return false;
                                      }

                                      contentType = r.get_header_value ("Content-Type");

                                      if (r.has_header ("Content-Length"))
                                      {
                                          try { total = (std::int64_t) std::stoull (r.get_header_value ("Content-Length")); }
                                          catch (...) { total = -1; }
                                      }

                                      if (opts.maxBytes > 0 && total >= 0 && (std::uint64_t) total > opts.maxBytes)
                                      {
                                          refused = makeError (errc::fileTooLarge, "the file is larger than the limit of " + std::to_string (opts.maxBytes) + " bytes", json { { "max_bytes", opts.maxBytes } });
                                          return false;
                                      }

                                      if (opts.accept)
                                          if (auto ok = opts.accept (contentType, total, current); ! ok.ok())
                                          {
                                              refused = ok.error();
                                              return false;
                                          }

                                      accepted = true;
                                      return true;
                                  },
                                  [&] (const char* data, size_t n)
                                  {
                                      if (opts.cancelled && opts.cancelled())
                                      {
                                          refused = makeError (errc::cancelled, "the download was cancelled");
                                          return false;
                                      }

                                      if (std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count() > opts.totalTimeoutS)
                                      {
                                          refused = makeError (errc::busy, "the download took longer than the time limit");
                                          return false;
                                      }

                                      received += n;

                                      if (opts.maxBytes > 0 && received > opts.maxBytes)
                                      {
                                          refused = makeError (errc::fileTooLarge, "the file is larger than the limit of " + std::to_string (opts.maxBytes) + " bytes", json { { "max_bytes", opts.maxBytes } });
                                          return false;
                                      }

                                      sink.write (data, (std::streamsize) n);

                                      if (! sink.good())
                                      {
                                          refused = makeError (errc::io, "cannot write the download file");
                                          return false;
                                      }

                                      if (opts.progress)
                                          opts.progress (received, total);

                                      return true;
                                  });

        sink.close();

        if (! redirectTo.empty())
        {
            const auto next = redirectTarget (url, redirectTo);

            if (next.empty())
                return fail (makeError (errc::preconditionFailed, "the redirect has no usable Location"));

            current = next;
            continue;
        }

        if (refused)
            return fail (*refused);

        if (! result || ! accepted)
            return fail (makeError (errc::busy, "the download failed (no usable HTTP answer)"));

        if (total >= 0 && received != (std::uint64_t) total)
            return fail (makeError (errc::busy, "the download ended before the announced size"));

        FetchResult out;
        out.finalUrl = current;
        out.contentType = contentType;
        out.bytes = received;
        return out;
    }

    return fail (makeError (errc::preconditionFailed, "too many redirects"));
}

} // namespace rovox
