// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// One-time signed links into the web UI (E9-01): `GET /open?project=<id>&token=<token>`. The platform mints the token, the core only verifies it.
//
//   [open_link]
//   secret_ref = "ROVOX_OPEN_SECRET"     # name of an environment variable (or `secret = "..."` inline); no secret = the route is off
//
// token = <hex of the claims JSON>.<hex HMAC-SHA256(secret, "open|" + <hex of the claims JSON>)>
// claims = {"v":1, "tenant", "project", "subject", "client", "scopes":[...], "exp":<unix>, "jti":"<nonce>"}
// The link is valid until `exp` (at most 5 minutes from now: a longer-lived token is refused) and for ONE redemption: the nonce is kept in
// <data>/auth/open-used.log until it expires, also across a restart. The grant is bound to the tenant and the project of the token; the
// secret and the token never appear in errors or logs.
#pragma once

#include "Auth.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace rovox
{

struct OpenGrant
{
    std::string tenant, project, subject, client;
    ScopeSet scopes = ScopeSet::of ({ Scope::catalogRead, Scope::projectsRead, Scope::projectsWrite, Scope::render, Scope::delivery });
    std::int64_t exp = 0;
    std::string jti;
};

class OpenLinks
{
public:
    static constexpr std::int64_t maxLifetimeS = 300;

    /** `configFile` is rovox.toml (may be empty or missing: the links are off); the used-nonce log lives under <dataDir>/auth/. */
    OpenLinks (std::filesystem::path dataDir, std::filesystem::path configFile);

    /** True when a shared secret is configured. */
    bool enabled() const;

    /** Checks signature, expiry, lifetime, tenant, and that `project` is the one the token names; then burns the nonce.
        UNAUTHORIZED for every failure (one answer, no detail on which check failed); a second redemption of the same token fails. */
    Result<OpenGrant> redeem (const std::string& token, const std::string& project, std::int64_t now);

    /** The token for `grant` (what the platform does; used by tests and tools). Empty for an empty secret. */
    static std::string mint (const std::string& secret, const OpenGrant& grant);

private:
    std::string secret() const;
    bool burn (const std::string& jti, std::int64_t exp, std::int64_t now);

    std::filesystem::path usedFile_, configFile_;
    std::mutex mutex_;
    bool loaded_ = false;
    std::map<std::string, std::int64_t> used_;      // jti -> exp
};

} // namespace rovox
