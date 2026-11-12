// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The one http(s) URL parser of the server (delivery adapters, S3 storage, job callbacks).
#pragma once

#include <string>

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

} // namespace rovox
