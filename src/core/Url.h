// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The one http(s) URL parser of the server (delivery adapters, S3 storage, job callbacks) and the one SSRF guard (job callbacks, upload.from_url):
// addresses are judged after DNS resolution, and a download connects to the address that was checked (never to a second DNS answer), on every redirect.
#pragma once

#include "Result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rovox
{

/** An http(s) URL split into parts. `host` is bare (an IPv6 literal without brackets); `origin` is scheme://host[:port] with the
    brackets and the port as written; `hostHeader` is host[:port] as a Host header wants it (default port omitted); `path` is the
    path with its query ("" if there is none, fragment dropped). `valid` is false, with a reason in `error`, for another scheme,
    an empty host, user info, a malformed IPv6 literal or a port that is not 1..65535. */
struct Url
{
    std::string scheme, host, path, origin, hostHeader, error;
    int port = 0;
    bool valid = false;
};

Url parseUrl (const std::string& url);

/** True for loopback, private (RFC 1918), link-local (incl. the 169.254.169.254 metadata address), unique-local (fc00::/7), unspecified, multicast, carrier-grade NAT (100.64/10) and benchmarking (198.18/15) addresses;
    IPv4-compatible (::a.b.c.d) always, IPv4-mapped, NAT64 (64:ff9b::/96) and 6to4 (2002::/16) by the IPv4 address they carry. Anything that is not an address is "private". */
bool isPrivateAddress (const std::string& ip);

/** 127.0.0.0/8, ::1 and ::ffff:127.x.x.x. */
bool isLoopbackAddress (const std::string& ip);

/** Resolves a host to numeric addresses; the default is getaddrinfo. Tests inject one. */
using HostResolver = std::function<std::vector<std::string> (const std::string& host, int port)>;

/** Every address the host resolves to (numeric strings); empty if it does not resolve. */
std::vector<std::string> resolveHost (const std::string& host, int port);

struct UrlGuard
{
    HostResolver resolver;              // null = getaddrinfo
    bool allowPrivate = false;
    bool allowLoopback = false;         // test-only: lets 127.0.0.0/8 and ::1 through, everything else stays blocked. Nothing in a request can set it.
};

/** Resolves the host of an already parsed URL and refuses it if one of its addresses is blocked. Returns the addresses (the first one is the one to connect to).
    INVALID_PARAMS for an unresolvable host or a blocked address. */
Result<std::vector<std::string>> checkUrlHost (const Url&, const UrlGuard&);

struct FetchOptions
{
    std::uint64_t maxBytes = 0;         // 0 = no limit
    int maxRedirects = 3;
    int connectTimeoutS = 10;
    int readTimeoutS = 30;
    double totalTimeoutS = 3600;        // a slow trickle cannot hold the job forever
    UrlGuard guard;

    /** Called with the headers of the final 200 answer (Content-Length is -1 when unknown). An error aborts the download and is returned as it is. */
    std::function<Result<void> (const std::string& contentType, std::int64_t contentLength, const std::string& finalUrl)> accept;
    std::function<bool()> cancelled;
    std::function<void (std::uint64_t received, std::int64_t total)> progress;
};

struct FetchResult
{
    std::string finalUrl, contentType;
    std::uint64_t bytes = 0;
};

/** GET `url` into the file `dest` (replaced). Only http(s); the guard is applied to the first URL and to every redirect target, at most maxRedirects redirects;
    the body is cut off (FILE_TOO_LARGE) past maxBytes. A partial file is removed on any failure. */
Result<FetchResult> fetchUrlToFile (const std::string& url, const std::filesystem::path& dest, const FetchOptions&);

} // namespace rovox
