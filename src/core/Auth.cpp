// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Auth.h"

#include "KeyStore.h"
#include "Sha256.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace rovox
{

namespace
{
constexpr std::string_view keyPrefix = "rvx_";
constexpr size_t idChars = 10, secretChars = 43;
constexpr std::string_view base32 = "abcdefghijklmnopqrstuvwxyz234567";
constexpr std::string_view base64url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

void randomBytes (unsigned char* out, int n)
{
    if (RAND_bytes (out, n) != 1)
        throw std::runtime_error ("the system random source failed");
}

bool onlyFrom (std::string_view s, std::string_view alphabet)
{
    return std::all_of (s.begin(), s.end(), [&] (char c) { return alphabet.find (c) != std::string_view::npos; });
}

std::string trimmed (std::string_view s)
{
    while (! s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix (1);

    while (! s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix (1);

    return std::string (s);
}

std::string stripBearer (std::string_view v)
{
    auto s = trimmed (v);

    if (s.size() >= 7 && (s[0] == 'B' || s[0] == 'b') && std::equal (s.begin() + 1, s.begin() + 6, "earer") && s[6] == ' ')
        return trimmed (std::string_view (s).substr (7));

    return s;
}

Error unauthorized()
{
    return makeError (errc::unauthorized, "missing or invalid credentials");
}
} // namespace

//==============================================================================
const char* scopeName (Scope s)
{
    switch (s)
    {
        case Scope::catalogRead:    return "catalog:read";
        case Scope::projectsRead:   return "projects:read";
        case Scope::projectsWrite:  return "projects:write";
        case Scope::render:         return "render";
        case Scope::delivery:       return "delivery";
        case Scope::admin:          return "admin";
    }

    return "";
}

ScopeSet ScopeSet::all()
{
    ScopeSet s;

    for (auto v : allScopeValues)
        s.add (v);

    return s;
}

ScopeSet ScopeSet::of (std::initializer_list<Scope> list)
{
    ScopeSet s;

    for (auto v : list)
        s.add (v);

    return s;
}

Result<ScopeSet> ScopeSet::parse (const std::vector<std::string>& names)
{
    ScopeSet out;

    for (auto& n : names)
    {
        bool found = false;

        for (auto v : allScopeValues)
            if (n == scopeName (v))
            {
                out.add (v);
                found = true;
            }

        if (n == "read")
        {
            out.add (Scope::catalogRead);
            out.add (Scope::projectsRead);
            found = true;
        }
        else if (n == "write")
        {
            out.add (Scope::catalogRead);
            out.add (Scope::projectsRead);
            out.add (Scope::projectsWrite);
            found = true;
        }

        if (! found)
            return makeError (errc::invalidParams, "unknown scope " + n);
    }

    return out;
}

std::vector<std::string> ScopeSet::names() const
{
    std::vector<std::string> out;

    for (auto v : allScopeValues)
        if (has (v))
            out.push_back (scopeName (v));

    return out;
}

//==============================================================================
bool constantTimeEquals (std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;

    unsigned char diff = 0;

    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char) (a[i] ^ b[i]);

    return diff == 0;
}

//==============================================================================
std::string newKeyId()
{
    std::array<unsigned char, idChars> raw {};
    randomBytes (raw.data(), (int) raw.size());
    std::string id = "k_";

    for (auto b : raw)
        id += base32[b & 31];

    return id;
}

std::string newKeySecret()
{
    std::array<unsigned char, 32> raw {};
    randomBytes (raw.data(), (int) raw.size());
    std::string out;

    // base64url without padding: 32 bytes -> 43 characters
    for (size_t i = 0; i < raw.size(); i += 3)
    {
        const size_t n = std::min<size_t> (3, raw.size() - i);
        std::uint32_t v = (std::uint32_t) raw[i] << 16;

        if (n > 1)
            v |= (std::uint32_t) raw[i + 1] << 8;

        if (n > 2)
            v |= raw[i + 2];

        out += base64url[(v >> 18) & 63];
        out += base64url[(v >> 12) & 63];

        if (n > 1)
            out += base64url[(v >> 6) & 63];

        if (n > 2)
            out += base64url[v & 63];
    }

    return out;
}

std::string formatApiKey (const std::string& id, const std::string& secret)
{
    return std::string (keyPrefix) + id + "_" + secret;
}

std::optional<ParsedKey> parseApiKey (std::string_view key)
{
    // rvx_ + "k_" + 10 chars + "_" + 43 chars
    if (key.size() != keyPrefix.size() + 2 + idChars + 1 + secretChars || key.substr (0, keyPrefix.size()) != keyPrefix)
        return std::nullopt;

    key.remove_prefix (keyPrefix.size());
    const auto id = key.substr (0, 2 + idChars);

    if (id.substr (0, 2) != "k_" || ! onlyFrom (id.substr (2), base32) || key[id.size()] != '_')
        return std::nullopt;

    const auto secret = key.substr (id.size() + 1);

    if (! onlyFrom (secret, base64url))
        return std::nullopt;

    return ParsedKey { std::string (id), std::string (secret) };
}

std::string hashKeySecret (const std::string& secret)
{
    return "sha256:" + sha256Hex (secret);
}

//==============================================================================
Authenticator::Authenticator (KeyStore* keys, std::string legacyToken) : keys_ (keys), legacyToken_ (std::move (legacyToken)) {}

bool Authenticator::open() const
{
    return legacyToken_.empty() && (keys_ == nullptr || ! keys_->inUse());
}

Result<Principal> Authenticator::resolve (std::string_view presented) const
{
    if (open())
    {
        Principal p = Principal::local();
        p.authMode = "open";
        return p;
    }

    const auto token = stripBearer (presented);

    if (token.empty())
        return unauthorized();

    if (! legacyToken_.empty() && constantTimeEquals (token, legacyToken_))
    {
        Principal p = Principal::local();
        p.keyId = "legacy";
        p.authMode = "legacy";
        return p;
    }

    if (keys_ == nullptr)
        return unauthorized();

    auto parsed = parseApiKey (token);

    if (! parsed)
        return unauthorized();

    const auto rec = keys_->find (parsed->id);
    const auto hash = hashKeySecret (parsed->secret);
    // an unknown id still costs one comparison, so the answer time does not tell whether the id exists
    const bool match = constantTimeEquals (hash, rec ? rec->hash : std::string ("sha256:") + std::string (64, '0'));

    if (! rec || ! match || rec->revoked() || rec->expired (keys_->now()))
        return unauthorized();

    keys_->touch (rec->id);
    Principal p;
    p.tenant = rec->tenant;
    p.keyId = rec->id;
    p.scopes = rec->scopes;
    p.quotaProfile = rec->quotaProfile;
    p.authMode = "token";
    return p;
}

bool Authenticator::stillValid (const Principal& p) const
{
    if (p.authMode != "token")
        return true;

    if (keys_ == nullptr)
        return false;

    const auto rec = keys_->find (p.keyId);
    return rec && ! rec->revoked() && ! rec->expired (keys_->now());
}

} // namespace rovox
