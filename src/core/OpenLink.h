// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// One-time signed links into the web UI (E9-01): `GET /open?project=<id>&token=<token>`. The platform mints the token, the core only verifies it.
//
//   [open_link]
//   secret_ref = "ROVOX_OPEN_SECRET"     # name of an environment variable (or `secret = "..."` inline); no secret = the route is off
//   epoch = 0                            # bump it to end every session minted from a link (the epoch travels in the session)
//   session_s = 3600                     # length of the session a link starts (60 .. 28800)
//
// token = <hex of the claims JSON>.<hex HMAC-SHA256(secret, "open|" + <hex of the claims JSON>)>
// claims = {"v":1, "tenant", "project", "subject", "client", "scopes":[...], "exp":<unix>, "jti":"<nonce>"}   (scopes: an explicit non-empty array, never defaulted)
// The link is valid until `exp` (at most 5 minutes from now: a longer-lived token is refused) and for ONE redemption: the nonce is kept in
// <data>/auth/open-used.log until it expires, also across a restart (the log is compacted when it grows). The grant is bound to the tenant and
// the project of the token, and so is the session it starts (it is revoked by removing the secret or bumping the epoch); the secret and the token
// never appear in errors or logs.
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
    ScopeSet scopes;                // always explicit: redeem refuses a token without a scopes array
    std::int64_t exp = 0;
    std::string jti;
};

/** The `[open_link]` table of rovox.toml. An empty secret = the links are off. */
struct OpenLinkConfig
{
    std::string secret;
    std::int64_t epoch = 0;
    std::int64_t sessionS = 3600;
};

class OpenLinks
{
public:
    static constexpr std::int64_t maxLifetimeS = 300;
    static constexpr std::int64_t defaultSessionS = 3600;
    static constexpr size_t maxIdentityBytes = 256;       // subject and client

    /** Reads `[open_link]` (a missing or broken file gives an empty secret: fails closed). Used by the session check too. */
    static OpenLinkConfig readConfig (const std::filesystem::path& configFile);
    OpenLinkConfig config() const       { return readConfig (configFile_); }

    /** `configFile` is rovox.toml (may be empty or missing: the links are off); the used-nonce log lives under <dataDir>/auth/. */
    OpenLinks (std::filesystem::path dataDir, std::filesystem::path configFile);

    /** True when a shared secret is configured. */
    bool enabled() const;

    /** Checks signature, expiry, lifetime, tenant (not "local"), identity length, an explicit scopes array, and that `project` is the one the token names; then burns the nonce.
        UNAUTHORIZED for every failure (one answer, no detail on which check failed); a second redemption of the same token fails. */
    Result<OpenGrant> redeem (const std::string& token, const std::string& project, std::int64_t now);

    /** The token for `grant` (what the platform does; used by tests and tools). Empty for an empty secret. */
    static std::string mint (const std::string& secret, const OpenGrant& grant);

private:
    std::string secret() const;
    bool burn (const std::string& jti, std::int64_t exp, std::int64_t now);
    void compact();

    std::filesystem::path usedFile_, configFile_;
    std::mutex mutex_;
    bool loaded_ = false;
    size_t lines_ = 0;                              // lines in the log file
    std::map<std::string, std::int64_t> used_;      // jti -> exp
};

} // namespace rovox
