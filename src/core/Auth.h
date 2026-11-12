// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Who is calling: scopes, the Principal carried by every request, and the Authenticator that turns a presented secret
// (legacy server token or an API key `rvx_<id>_<secret>`) into one. No transport wiring lives here.
#pragma once

#include "Result.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rovox
{

class KeyStore;

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
    std::string authMode = "local"; // local | open | legacy | token

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
class Authenticator
{
public:
    /** `keys` may be null (no key store); `legacyToken` empty = none. Neither is owned beyond the call lifetime of the Authenticator. */
    Authenticator (KeyStore* keys, std::string legacyToken);

    /** True when there is no legacy token and no keys: every request is the local operator (today's loopback behaviour). */
    bool open() const;

    /** `presented` is the Authorization header value ("Bearer ...") or a raw token; empty = nothing presented.
        Fails with UNAUTHORIZED; neither the message nor the details ever contain the secret. */
    Result<Principal> resolve (std::string_view presented) const;

    /** True if the principal is still valid (key neither revoked nor expired); for long-lived connections (WS, MCP sessions). */
    bool stillValid (const Principal&) const;

private:
    KeyStore* keys_;
    std::string legacyToken_;
};

} // namespace rovox
