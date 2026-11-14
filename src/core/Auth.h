// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Who is calling: scopes, the Principal carried by every request, and the Authenticator that turns a presented secret
// (legacy server token or an API key `rvx_<id>_<secret>`) into one. No transport wiring lives here.
#pragma once

#include "Result.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rovox
{

class KeyStore;
class ExternalAuth;
struct KeyRecord;

enum class Scope : std::uint32_t
{
    catalogRead   = 1u << 0,
    projectsRead  = 1u << 1,
    projectsWrite = 1u << 2,
    render        = 1u << 3,
    delivery      = 1u << 4,
    admin         = 1u << 5,
};

/** "catalog:read", "projects:read", "projects:write", "render", "delivery", "admin". */
const char* scopeName (Scope);
inline constexpr Scope allScopeValues[] = { Scope::catalogRead, Scope::projectsRead, Scope::projectsWrite, Scope::render, Scope::delivery, Scope::admin };

class ScopeSet
{
public:
    ScopeSet() = default;
    explicit ScopeSet (std::uint32_t bits) : bits_ (bits) {}

    static ScopeSet all();
    static ScopeSet of (std::initializer_list<Scope>);
    /** Names as in scopeName(); the MVP aliases `read` (catalog:read + projects:read) and `write` (read + projects:write) are accepted too. */
    static Result<ScopeSet> parse (const std::vector<std::string>& names);

    bool has (Scope s) const        { return (bits_ & (std::uint32_t) s) != 0; }
    void add (Scope s)              { bits_ |= (std::uint32_t) s; }
    void remove (Scope s)           { bits_ &= ~(std::uint32_t) s; }
    bool empty() const              { return bits_ == 0; }
    std::uint32_t bits() const      { return bits_; }
    std::vector<std::string> names() const;
    bool operator== (const ScopeSet& o) const   { return bits_ == o.bits_; }

private:
    std::uint32_t bits_ = 0;
};

struct Principal
{
    std::string tenant = "local";
    std::string keyId;              // empty for the local operator and for open mode
    std::string subject;            // end user behind the key (external auth, E9-01)
    std::string client;             // calling application
    ScopeSet scopes = ScopeSet::all();
    std::string quotaProfile;       // empty = the tenant's default; `local` is unlimited
    std::string authMode = "local"; // local | open | legacy | token | trusted_proxy | introspection | open_link
    std::string project;            // non-empty = the principal may touch this one project only (a session minted from an /open link)
    std::int64_t epoch = 0;         // open_link: the [open_link] epoch the session was minted under (stillValid compares it with the config)
    std::int64_t expiresAt = 0;     // unix seconds; 0 = none. External principals: a long-lived connection is dropped after it (stillValid)

    bool has (Scope s) const        { return scopes.has (s); }

    /** The operator: tenant "local", every scope. The default everywhere a request carries no authentication. */
    static Principal local()        { return {}; }
};

/** Constant-time equality (the length is not hidden). */
bool constantTimeEquals (std::string_view a, std::string_view b);

//==============================================================================
/** A parsed `rvx_<id>_<secret>` key: id = "k_" + 10 base32 characters, secret = 43 base64url characters (32 random bytes). */
struct ParsedKey
{
    std::string id, secret;
};

std::optional<ParsedKey> parseApiKey (std::string_view key);
std::string formatApiKey (const std::string& id, const std::string& secret);
/** "sha256:<hex of SHA-256(secret)>". */
std::string hashKeySecret (const std::string& secret);
/** A fresh key id ("k_" + 10 base32) and secret from the OpenSSL CSPRNG. */
std::string newKeyId();
std::string newKeySecret();

//==============================================================================
/** Everything a transport knows about the credentials of one request. Fields that do not apply stay empty. */
struct AuthRequest
{
    std::string authorization;                      // the Authorization header value ("Bearer ...")
    std::string cookie;                             // the Cookie header
    std::string queryToken;                         // ?token= (only where the transport allows it)
    std::string remoteAddr, method, path;
    std::string target;                             // request target (path + "?" + query); empty = just `path`. Covered by the trusted-proxy signature
    std::map<std::string, std::string> headers;     // lower-case names
};

class Authenticator
{
public:
    /** `keys` may be null (no key store); `legacyToken` empty = none. Neither is owned beyond the call lifetime of the Authenticator.
        `configFile` (rovox.toml) holds `[auth] mode = token | trusted_proxy | introspection`, see ExternalAuth.h; native keys
        `rvx_...` are valid in the mode `token` only, the legacy token in every mode. */
    Authenticator (KeyStore* keys, std::string legacyToken, std::filesystem::path configFile = {});
    ~Authenticator();

    /** True when there is no legacy token and no keys: every request is the local operator (today's loopback behaviour). */
    bool open() const;

    /** Set by serve when the bind is not loopback: from then on open() is false even if keys.json disappears. It also latches by itself once keys were seen. */
    void setRequireCredentials (bool on);

    /** `presented` is the Authorization header value ("Bearer ...") or a raw token; empty = nothing presented.
        Fails with UNAUTHORIZED; neither the message nor the details ever contain the secret. */
    Result<Principal> resolve (std::string_view presented) const;

    /** Tries, in order: the Authorization header, the signed `rovox_session` cookie, the old `rovox_token` cookie (legacy token only), `?token=`. */
    Result<Principal> resolve (const AuthRequest&) const;

    /** True if the principal is still valid (key neither revoked nor expired); for long-lived connections (WS, MCP sessions). */
    bool stillValid (const Principal&) const;

    /** Where the secret of the session cookie lives (<data>/auth/session.key, 32 random bytes, 0600). Without a file the secret is kept in memory only. */
    void setSessionKeyFile (std::filesystem::path file);

    /** `<key_id>.<exp>.<hex HMAC-SHA256>` for `rovox_session`; empty for the open mode (nothing to log in to). */
    std::string issueSession (const Principal&, std::int64_t ttlSeconds = 30 * 24 * 3600) const;

    /** The `rovox_session` value for a principal that redeemed a one-time /open link (tenant, project, epoch, subject, client and scopes travel in the signed value);
        `fromSession` turns it back into a Principal with `authMode` "open_link". */
    std::string issueOpenSession (const Principal&, std::int64_t ttlSeconds) const;

    /** Value of `WWW-Authenticate` on a 401; empty unless the mode is introspection with a `resource`. */
    std::string challengeHeader() const;
    /** OAuth protected-resource metadata (RFC 9728); nullopt = the route is not registered. */
    std::optional<json> resourceMetadata() const;

    const std::filesystem::path& configFile() const     { return configFile_; }

    /** The external auth modes (config, caches, test hooks for the HTTP client and the clock). */
    ExternalAuth& external() const                      { return *external_; }

private:
    std::string sessionKey() const;                     // creates the secret on first use
    Result<Principal> fromSession (std::string_view cookieValue) const;
    Principal principalOf (const KeyRecord&) const;

    KeyStore* keys_;
    std::string legacyToken_;
    std::filesystem::path configFile_;
    std::unique_ptr<ExternalAuth> external_;
    mutable std::mutex keyMutex_;
    std::filesystem::path sessionKeyFile_;
    mutable std::string sessionKey_;
    mutable std::atomic<bool> requireCredentials_ { false };
};

/** The credentials of one HTTP request (httplib::Request or alike) as an AuthRequest: headers copied with lower-case names; `?token=` only for GET. */
template <class Request>
AuthRequest authRequestOf (const Request& req)
{
    AuthRequest a;
    a.authorization = req.get_header_value ("Authorization");
    a.cookie = req.get_header_value ("Cookie");
    a.remoteAddr = req.remote_addr;
    a.method = req.method;
    a.path = req.path;
    a.target = req.target;

    // `?token=` opens the page URL (GET) only; it is never a credential for a state-changing request
    if (req.method == "GET" && req.has_param ("token"))
        a.queryToken = req.get_param_value ("token");

    for (const auto& h : req.headers)
    {
        auto name = h.first;
        std::transform (name.begin(), name.end(), name.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        a.headers[name] = h.second;
    }

    return a;
}

} // namespace rovox
