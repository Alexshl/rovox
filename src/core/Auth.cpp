// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Auth.h"

#include "Hmac.h"
#include "KeyStore.h"
#include "Sha256.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

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

/** The value of cookie `name` in a Cookie header ("a=1; b=2"), empty if absent. */
std::string cookieValueOf (std::string_view cookies, std::string_view name)
{
    size_t pos = 0;

    while (pos < cookies.size())
    {
        const auto end = cookies.find (';', pos);
        auto item = cookies.substr (pos, end == std::string_view::npos ? std::string_view::npos : end - pos);

        while (! item.empty() && item.front() == ' ')
            item.remove_prefix (1);

        if (item.size() > name.size() && item.substr (0, name.size()) == name && item[name.size()] == '=')
            return std::string (item.substr (name.size() + 1));

        if (end == std::string_view::npos)
            break;

        pos = end + 1;
    }

    return {};
}

Principal legacyPrincipal()
{
    Principal p = Principal::local();
    p.keyId = "legacy";
    p.authMode = "legacy";
    return p;
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
Authenticator::Authenticator (KeyStore* keys, std::string legacyToken, std::filesystem::path configFile)
    : keys_ (keys), legacyToken_ (std::move (legacyToken)), configFile_ (std::move (configFile)) {}

bool Authenticator::open() const
{
    return legacyToken_.empty() && (keys_ == nullptr || ! keys_->inUse());
}

Principal Authenticator::principalOf (const KeyRecord& rec) const
{
    Principal p;
    p.tenant = rec.tenant;
    p.keyId = rec.id;
    p.scopes = rec.scopes;
    p.quotaProfile = rec.quotaProfile;
    p.authMode = "token";
    return p;
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
        return legacyPrincipal();

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
    return principalOf (*rec);
}

Result<Principal> Authenticator::resolve (const AuthRequest& req) const
{
    if (open())
    {
        Principal p = Principal::local();
        p.authMode = "open";
        return p;
    }

    if (! req.authorization.empty())
        if (auto r = resolve (std::string_view (req.authorization)); r.ok())
            return r;

    if (! req.cookie.empty())
    {
        if (const auto session = cookieValueOf (req.cookie, "rovox_session"); ! session.empty())
            if (auto r = fromSession (session); r.ok())
                return r;

        // the cookie of the previous release held the raw token: accepted for the legacy token only
        if (const auto old = cookieValueOf (req.cookie, "rovox_token"); ! old.empty() && ! legacyToken_.empty() && constantTimeEquals (old, legacyToken_))
            return legacyPrincipal();
    }

    if (! req.queryToken.empty())
        if (auto r = resolve (std::string_view (req.queryToken)); r.ok())
            return r;

    return unauthorized();
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

//==============================================================================
void Authenticator::setSessionKeyFile (std::filesystem::path file)
{
    std::lock_guard<std::mutex> lock (keyMutex_);
    sessionKeyFile_ = std::move (file);
    sessionKey_.clear();
}

std::string Authenticator::sessionKey() const
{
    std::lock_guard<std::mutex> lock (keyMutex_);

    if (! sessionKey_.empty())
        return sessionKey_;

    std::error_code ec;

    if (! sessionKeyFile_.empty() && std::filesystem::is_regular_file (sessionKeyFile_, ec) && std::filesystem::file_size (sessionKeyFile_, ec) == 32)
    {
        std::ifstream in (sessionKeyFile_, std::ios::binary);
        std::string k (32, '\0');
        in.read (k.data(), 32);

        if (in.gcount() == 32)
            return sessionKey_ = k;
    }

    std::string k (32, '\0');
    randomBytes (reinterpret_cast<unsigned char*> (k.data()), 32);

    if (! sessionKeyFile_.empty())
    {
        // atomic: a temporary file next to the target, mode 0600 from the start, then rename
        std::filesystem::create_directories (sessionKeyFile_.parent_path(), ec);
        const auto tmp = sessionKeyFile_.string() + ".tmp" + std::to_string ((long) ::getpid());
        const int fd = ::open (tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

        if (fd >= 0)
        {
            const bool written = ::write (fd, k.data(), k.size()) == (ssize_t) k.size();
            ::close (fd);

            if (written)
                std::filesystem::rename (tmp, sessionKeyFile_, ec);
            else
                std::filesystem::remove (tmp, ec);
        }
    }

    return sessionKey_ = k;
}

std::string Authenticator::issueSession (const Principal& p, std::int64_t ttlSeconds) const
{
    if (p.keyId.empty())
        return {};

    const std::int64_t now = keys_ != nullptr ? keys_->now() : (std::int64_t) std::time (nullptr);
    const auto body = p.keyId + "." + std::to_string (now + ttlSeconds);
    return body + "." + hmacSha256Hex (sessionKey(), body);
}

Result<Principal> Authenticator::fromSession (std::string_view value) const
{
    const auto dot1 = value.find ('.');
    const auto dot2 = dot1 == std::string_view::npos ? std::string_view::npos : value.find ('.', dot1 + 1);

    if (dot2 == std::string_view::npos)
        return unauthorized();

    const std::string body (value.substr (0, dot2));
    const std::string keyId (value.substr (0, dot1));

    if (! constantTimeEquals (value.substr (dot2 + 1), hmacSha256Hex (sessionKey(), body)))
        return unauthorized();

    std::int64_t exp = 0;

    try { exp = std::stoll (std::string (value.substr (dot1 + 1, dot2 - dot1 - 1))); }
    catch (...) { return unauthorized(); }

    const std::int64_t now = keys_ != nullptr ? keys_->now() : (std::int64_t) std::time (nullptr);

    if (now >= exp)
        return unauthorized();

    if (keyId == "legacy")
        return legacyToken_.empty() ? Result<Principal> (unauthorized()) : Result<Principal> (legacyPrincipal());

    if (keys_ == nullptr)
        return unauthorized();

    const auto rec = keys_->find (keyId);

    if (! rec || rec->revoked() || rec->expired (keys_->now()))
        return unauthorized();

    return principalOf (*rec);
}

std::string Authenticator::challengeHeader() const
{
    return {};
}

std::optional<json> Authenticator::resourceMetadata() const
{
    return std::nullopt;
}

} // namespace rovox
