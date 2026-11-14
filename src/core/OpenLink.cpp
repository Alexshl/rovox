// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "OpenLink.h"

#include "Hmac.h"
#include "Ids.h"

#include <toml++/toml.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
Error refused()
{
    return makeError (errc::unauthorized, "the link is invalid, expired or already used");
}

std::string hexOf (const std::string& s)
{
    return toHex (reinterpret_cast<const std::uint8_t*> (s.data()), s.size());
}

std::optional<std::string> unhex (const std::string& s)
{
    if (s.empty() || s.size() % 2 != 0)
        return std::nullopt;

    auto nib = [] (char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };

    std::string out;

    for (size_t i = 0; i < s.size(); i += 2)
    {
        const int a = nib (s[i]), b = nib (s[i + 1]);

        if (a < 0 || b < 0)
            return std::nullopt;

        out += (char) (a * 16 + b);
    }

    return out;
}

std::string macOf (const std::string& secret, const std::string& payloadHex)
{
    return hmacSha256Hex (secret, "open|" + payloadHex);
}

bool validNonce (const std::string& s)
{
    return s.size() >= 8 && s.size() <= 64
        && std::all_of (s.begin(), s.end(), [] (char c) { return std::isalnum ((unsigned char) c) != 0 || c == '_' || c == '-'; });
}
} // namespace

OpenLinks::OpenLinks (fs::path dataDir, fs::path configFile)
    : usedFile_ (dataDir / "auth" / "open-used.log"), configFile_ (std::move (configFile)) {}

std::string OpenLinks::secret() const
{
    std::error_code ec;

    if (configFile_.empty() || ! fs::exists (configFile_, ec))
        return {};

    try
    {
        const auto doc = toml::parse_file (configFile_.string());
        const auto* t = doc["open_link"].as_table();

        if (t == nullptr)
            return {};

        if (const auto ref = (*t)["secret_ref"].value_or (std::string()); ! ref.empty())
        {
            const char* v = std::getenv (ref.c_str());
            return v != nullptr ? std::string (v) : std::string();
        }

        return (*t)["secret"].value_or (std::string());
    }
    catch (const toml::parse_error&)
    {
        return {};      // fails closed
    }
}

bool OpenLinks::enabled() const
{
    return ! secret().empty();
}

std::string OpenLinks::mint (const std::string& secret, const OpenGrant& g)
{
    if (secret.empty())
        return {};

    json claims { { "v", 1 }, { "tenant", g.tenant }, { "project", g.project }, { "subject", g.subject }, { "client", g.client },
                  { "scopes", g.scopes.names() }, { "exp", g.exp }, { "jti", g.jti } };
    const auto payload = hexOf (claims.dump());
    return payload + "." + macOf (secret, payload);
}

bool OpenLinks::burn (const std::string& jti, std::int64_t exp, std::int64_t now)
{
    // the caller holds mutex_
    if (! loaded_)
    {
        loaded_ = true;
        std::ifstream in (usedFile_);
        std::string line;

        while (std::getline (in, line))
        {
            std::istringstream ls (line);
            std::string id;
            std::int64_t e = 0;

            if ((ls >> id >> e) && e > now)
                used_[id] = e;
        }
    }

    for (auto it = used_.begin(); it != used_.end();)
        it = it->second <= now ? used_.erase (it) : std::next (it);

    if (used_.count (jti) != 0)
        return false;

    used_[jti] = exp;
    std::error_code ec;
    fs::create_directories (usedFile_.parent_path(), ec);
    const int fd = ::open (usedFile_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);

    if (fd < 0)
    {
        used_.erase (jti);
        return false;       // the nonce cannot be persisted: refuse rather than allow a replay after a restart
    }

    const auto line = jti + " " + std::to_string (exp) + "\n";
    const bool written = ::write (fd, line.data(), line.size()) == (ssize_t) line.size();
    ::fsync (fd);
    ::close (fd);

    if (! written)
    {
        used_.erase (jti);
        return false;
    }

    return true;
}

Result<OpenGrant> OpenLinks::redeem (const std::string& token, const std::string& project, std::int64_t now)
{
    const auto key = secret();

    if (key.empty())
        return refused();

    const auto dot = token.find ('.');

    if (dot == std::string::npos || token.find ('.', dot + 1) != std::string::npos || token.size() > 4096)
        return refused();

    const auto payloadHex = token.substr (0, dot);

    if (! constantTimeEquals (token.substr (dot + 1), macOf (key, payloadHex)))
        return refused();

    const auto text = unhex (payloadHex);
    const auto claims = text ? json::parse (*text, nullptr, false) : json();

    if (! claims.is_object() || claims.value ("v", 0) != 1)
        return refused();

    auto str = [&] (const char* k) { return claims.contains (k) && claims[k].is_string() ? claims[k].get<std::string>() : std::string(); };

    OpenGrant g;
    g.tenant = str ("tenant");
    g.project = str ("project");
    g.subject = str ("subject");
    g.client = str ("client");
    g.jti = str ("jti");
    g.exp = claims.contains ("exp") && claims["exp"].is_number_integer() ? claims["exp"].get<std::int64_t>() : 0;

    if (! isValidTenantId (g.tenant) || g.project.empty() || ! validNonce (g.jti) || g.exp <= now || g.exp > now + maxLifetimeS)
        return refused();

    if (! constantTimeEquals (g.project, project))
        return refused();       // the link opens the project it was minted for, nothing else

    if (claims.contains ("scopes"))
    {
        if (! claims["scopes"].is_array())
            return refused();

        std::vector<std::string> names;

        for (const auto& s : claims["scopes"])
        {
            if (! s.is_string())
                return refused();

            names.push_back (s.get<std::string>());
        }

        auto scopes = ScopeSet::parse (names);

        if (! scopes.ok() || scopes.value().has (Scope::admin))
            return refused();       // a link never grants the operator scope

        g.scopes = scopes.value();
    }

    std::lock_guard<std::mutex> lock (mutex_);

    if (! burn (g.jti, g.exp, now))
        return refused();

    return g;
}

} // namespace rovox
