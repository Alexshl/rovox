// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// External authentication behind the Authenticator (E11-08, E9-01): `[auth] mode = token | trusted_proxy | introspection`.
//
//   [auth]
//   mode = "trusted_proxy"
//   [auth.trusted_proxy]
//   allow = ["10.0.0.0/8", "fd00::/8"]
//   secret_ref = "ROVOX_PROXY_SECRET"      # name of an environment variable (or `secret = "..."` inline)
//   max_skew_s = 60
//   [auth.introspection]
//   url = "https://idp/introspect"          # RFC 7662 ...
//   client_id = "rovox"
//   client_secret_ref = "ROVOX_IDP_SECRET"
//   jwks_url = "https://idp/jwks.json"      # ... and/or JWT (RS256/ES256); issuer and audience are then required
//   issuer = "https://idp"
//   audience = "rovox"
//   resource = "https://rovox.example.com"  # RFC 9728 resource identifier
//   tenant_claim = "tenant"                 # also subject_claim ("sub"), scope_claim ("scope"), client_claim ("client_id")
//
// Replay of a signed proxy request inside max_skew_s is accepted (threat model of the tenancy plan, section 2): keep the
// proxy-to-core link on TLS or a private network. Secrets and tokens never appear in errors or logs.
#pragma once

#include "Auth.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace rovox
{

enum class AuthMode
{
    token,
    trustedProxy,
    introspection,
};

/** An address range: 10.0.0.0/8, fd00::/8, or a single address. */
struct Cidr
{
    bool v6 = false;
    std::array<std::uint8_t, 16> addr {};   // 4 bytes used for IPv4
    int bits = 0;
};

std::optional<Cidr> parseCidr (std::string_view text);
/** `address` is a bare IPv4/IPv6 address (an IPv4-mapped IPv6 address counts as IPv4); false when it does not parse. */
bool cidrContains (const Cidr&, std::string_view address);

/** status 0 = no answer (network error, timeout). */
struct HttpReply
{
    int status = 0;
    std::string body;
};

using HttpFetcher = std::function<HttpReply (const std::string& method, const std::string& url, const std::string& body,
                                              const std::map<std::string, std::string>& headers)>;

class ExternalAuth
{
public:
    using Clock = std::function<std::int64_t()>;    // unix seconds

    /** `configFile` empty or missing = mode token. The file is re-read when its mtime changes. */
    explicit ExternalAuth (std::filesystem::path configFile);

    /** For tests: replaces the HTTP client of introspection / JWKS and the clock. */
    void setFetcher (HttpFetcher);
    void setClock (Clock);

    AuthMode mode() const;

    /** trusted_proxy: nullopt when the request is not a proxy request (address outside the allowlist, or no X-Rovox-* headers):
        those headers are then ignored. Otherwise the verdict: a principal, or UNAUTHORIZED (bad signature, stale timestamp, ...). */
    std::optional<Result<Principal>> fromProxy (const AuthRequest&) const;

    /** introspection: an opaque token (RFC 7662) or a JWT; results are cached by sha256(token). */
    Result<Principal> fromBearer (const std::string& token) const;

    /** `Bearer resource_metadata="<resource>/.well-known/oauth-protected-resource"`; empty unless mode introspection with a resource. */
    std::string challengeHeader() const;
    std::optional<json> resourceMetadata() const;

    struct Config;      // internal
    struct Outcome;
    struct Jwk;

private:
    std::shared_ptr<const Config> config() const;
    Outcome introspect (const Config&, const std::string& token, std::int64_t now) const;
    Outcome verifyJwt (const Config&, const std::string& token, std::int64_t now) const;
    std::shared_ptr<const Jwk> findKey (const Config&, const std::string& kid, std::int64_t now, bool& fetchFailed) const;
    HttpReply fetch (const std::string& method, const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) const;
    std::int64_t now() const;

    struct CacheEntry
    {
        std::optional<Principal> principal;     // nullopt = a cached rejection
        std::int64_t expires = 0;
    };

    std::filesystem::path file_;
    mutable std::mutex mutex_;                  // config and cache
    mutable std::shared_ptr<const Config> config_;
    mutable bool loaded_ = false;
    mutable std::optional<std::filesystem::file_time_type> mtime_;
    mutable std::unordered_map<std::string, CacheEntry> cache_;

    mutable std::mutex jwksMutex_;
    mutable std::map<std::string, std::shared_ptr<const Jwk>> jwks_;
    mutable std::int64_t jwksFetchedAt_ = 0;
    mutable bool jwksLoaded_ = false;

    HttpFetcher fetcher_;
    Clock clock_;
};

} // namespace rovox
