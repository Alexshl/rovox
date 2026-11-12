// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "KeyStore.h"

#include "Ids.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace rovox
{

namespace
{
constexpr auto checkInterval = std::chrono::seconds (1);
constexpr size_t maxLabel = 120;

bool writeAll (int fd, const std::string& data)
{
    size_t done = 0;

    while (done < data.size())
    {
        const auto n = ::write (fd, data.data() + done, data.size() - done);

        if (n < 0)
            return false;

        done += (size_t) n;
    }

    return true;
}

std::optional<KeyRecord> recordFromJson (const json& j)
{
    if (! j.is_object() || ! j.contains ("id") || ! j["id"].is_string() || ! j.contains ("hash") || ! j["hash"].is_string())
        return std::nullopt;

    KeyRecord r;
    r.id = j["id"].get<std::string>();
    r.hash = j["hash"].get<std::string>();
    r.label = j.value ("label", std::string());
    r.tenant = j.value ("tenant", std::string ("local"));

    if (! isValidTenantId (r.tenant))
        return std::nullopt;

    std::vector<std::string> names;

    if (j.contains ("scopes") && j["scopes"].is_array())
        for (auto& s : j["scopes"])
            if (s.is_string())
                names.push_back (s.get<std::string>());

    if (auto scopes = ScopeSet::parse (names); scopes.ok())
        r.scopes = scopes.value();

    // `admin` belongs to the local tenant only, whatever a hand-edited file says
    if (r.tenant != "local")
        r.scopes.remove (Scope::admin);

    r.quotaProfile = j.value ("quota_profile", std::string());
    r.createdAt = j.value ("created_at", (std::int64_t) 0);
    r.lastUsedAt = j.value ("last_used_at", (std::int64_t) 0);

    if (j.contains ("expires_at") && j["expires_at"].is_number_integer())
        r.expiresAt = j["expires_at"].get<std::int64_t>();

    if (j.contains ("revoked_at") && j["revoked_at"].is_number_integer())
        r.revokedAt = j["revoked_at"].get<std::int64_t>();

    return r;
}

json recordToJson (const KeyRecord& r, std::int64_t lastUsed, bool withHash)
{
    json j { { "id", r.id }, { "label", r.label }, { "tenant", r.tenant }, { "scopes", r.scopes.names() }, { "created_at", r.createdAt },
             { "last_used_at", std::max (lastUsed, r.lastUsedAt) } };

    if (! r.quotaProfile.empty())
        j["quota_profile"] = r.quotaProfile;

    if (r.expiresAt)
        j["expires_at"] = *r.expiresAt;

    if (r.revokedAt)
        j["revoked_at"] = *r.revokedAt;

    if (withHash)
        j["hash"] = r.hash;

    return j;
}
} // namespace

json KeyRecord::toPublicJson() const
{
    return recordToJson (*this, lastUsedAt, false);
}

//==============================================================================
KeyStore::KeyStore (fs::path file, std::function<std::int64_t()> clock) : file_ (std::move (file)), clock_ (std::move (clock)) {}

std::int64_t KeyStore::now() const
{
    return clock_ ? clock_() : (std::int64_t) std::time (nullptr);
}

void KeyStore::refresh (bool force)
{
    const auto t = std::chrono::steady_clock::now();

    if (! force && loaded_ && t - lastCheck_ < checkInterval)
        return;

    lastCheck_ = t;
    std::error_code ec;

    if (! fs::exists (file_, ec))
    {
        if (! loaded_ || fileExisted_)
        {
            records_.clear();
            broken_ = false;
        }

        fileExisted_ = false;
        loaded_ = true;
        return;
    }

    const auto mt = fs::last_write_time (file_, ec);
    const auto sz = fs::file_size (file_, ec);

    if (loaded_ && fileExisted_ && mt == mtime_ && sz == size_)
        return;

    mtime_ = mt;
    size_ = sz;
    fileExisted_ = true;
    loaded_ = true;

    std::ifstream in (file_);
    std::stringstream ss;
    ss << in.rdbuf();
    const auto doc = json::parse (ss.str(), nullptr, false);

    if (doc.is_discarded() || ! doc.is_object() || ! doc.contains ("keys") || ! doc["keys"].is_array())
    {
        broken_ = true;         // keep the last good records; the server is not "open" while the file is unreadable
        return;
    }

    broken_ = false;
    std::map<std::string, KeyRecord> fresh;

    for (auto& j : doc["keys"])
        if (auto r = recordFromJson (j))
            fresh[r->id] = std::move (*r);

    records_ = std::move (fresh);
}

Result<void> KeyStore::save()
{
    json keys = json::array();

    for (auto& [id, r] : records_)
    {
        if (auto it = lastUsed_.find (id); it != lastUsed_.end())
            r.lastUsedAt = std::max (r.lastUsedAt, it->second);

        keys.push_back (recordToJson (r, r.lastUsedAt, true));
    }

    const std::string text = json { { "version", 1 }, { "keys", keys } }.dump (2) + "\n";
    std::error_code ec;
    fs::create_directories (file_.parent_path(), ec);
    ::chmod (file_.parent_path().c_str(), 0700);

    const auto tmp = file_.string() + ".tmp." + std::to_string ((long) ::getpid());
    const int fd = ::open (tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        return makeError (errc::io, "cannot write the key file");

    ::fchmod (fd, 0600);
    const bool ok = writeAll (fd, text) && ::fsync (fd) == 0;
    ::close (fd);

    if (! ok || ::rename (tmp.c_str(), file_.c_str()) != 0)
    {
        ::unlink (tmp.c_str());
        return makeError (errc::io, "cannot write the key file");
    }

    if (const int dirFd = ::open (file_.parent_path().c_str(), O_RDONLY); dirFd >= 0)
    {
        ::fsync (dirFd);
        ::close (dirFd);
    }

    // our own write is not a change to reload
    mtime_ = fs::last_write_time (file_, ec);
    size_ = fs::file_size (file_, ec);
    fileExisted_ = true;
    loaded_ = true;
    broken_ = false;
    return {};
}

Result<KeyStore::Created> KeyStore::createLocked (const CreateSpec& spec)
{
    if (! isValidTenantId (spec.tenant))
        return makeError (errc::invalidParams, "invalid tenant id");

    if (spec.scopes.empty())
        return makeError (errc::invalidParams, "a key needs at least one scope");

    if (spec.scopes.has (Scope::admin) && spec.tenant != "local")
        return makeError (errc::invalidParams, "the admin scope can only be granted to the local tenant");

    if (spec.label.size() > maxLabel)
        return makeError (errc::invalidParams, "label is too long");

    std::string id = newKeyId();

    while (records_.count (id) > 0)
        id = newKeyId();

    const auto secret = newKeySecret();
    KeyRecord r;
    r.id = id;
    r.label = spec.label;
    r.tenant = spec.tenant;
    r.scopes = spec.scopes;
    r.quotaProfile = spec.quotaProfile;
    r.createdAt = now();
    r.expiresAt = spec.expiresAt;
    r.hash = hashKeySecret (secret);
    records_[id] = r;
    return Created { r.toPublicJson(), formatApiKey (id, secret) };
}

Result<KeyStore::Created> KeyStore::create (const CreateSpec& spec)
{
    std::lock_guard<std::mutex> lock (mutex_);
    refresh (true);
    auto out = createLocked (spec);

    if (! out.ok())
        return out;

    if (auto saved = save(); ! saved.ok())
    {
        records_.erase (out.value().record["id"].get<std::string>());
        return saved.error();
    }

    return out;
}

json KeyStore::list()
{
    std::lock_guard<std::mutex> lock (mutex_);
    refresh (false);
    json out = json::array();

    for (auto& [id, r] : records_)
    {
        auto it = lastUsed_.find (id);
        out.push_back (recordToJson (r, it == lastUsed_.end() ? 0 : it->second, false));
    }

    return out;
}

Result<json> KeyStore::revoke (const std::string& id)
{
    std::lock_guard<std::mutex> lock (mutex_);
    refresh (true);
    auto it = records_.find (id);

    if (it == records_.end())
        return makeError (errc::notFound, "no key " + id);

    if (it->second.revoked())
        return it->second.toPublicJson();

    it->second.revokedAt = now();

    if (auto saved = save(); ! saved.ok())
    {
        it->second.revokedAt.reset();
        return saved.error();
    }

    return it->second.toPublicJson();
}

Result<KeyStore::Created> KeyStore::rotate (const std::string& id, std::int64_t graceSeconds)
{
    std::lock_guard<std::mutex> lock (mutex_);
    refresh (true);
    auto it = records_.find (id);

    if (it == records_.end())
        return makeError (errc::notFound, "no key " + id);

    const auto before = it->second;

    if (before.revoked() || before.expired (now()))
        return makeError (errc::preconditionFailed, "the key is already revoked or expired");

    CreateSpec spec;
    spec.label = before.label;
    spec.tenant = before.tenant;
    spec.scopes = before.scopes;
    spec.quotaProfile = before.quotaProfile;
    spec.expiresAt = before.expiresAt;
    auto created = createLocked (spec);

    if (! created.ok())
        return created;

    auto& old = records_[id];

    if (graceSeconds <= 0)
        old.revokedAt = now();
    else if (! old.expiresAt || *old.expiresAt > now() + graceSeconds)
        old.expiresAt = now() + graceSeconds;

    if (auto saved = save(); ! saved.ok())
    {
        records_.erase (created.value().record["id"].get<std::string>());
        records_[id] = before;
        return saved.error();
    }

    return created;
}

std::optional<KeyRecord> KeyStore::find (const std::string& id)
{
    std::lock_guard<std::mutex> lock (mutex_);
    refresh (false);
    auto it = records_.find (id);

    if (it == records_.end())
        return std::nullopt;

    auto r = it->second;

    if (auto used = lastUsed_.find (id); used != lastUsed_.end())
        r.lastUsedAt = std::max (r.lastUsedAt, used->second);

    return r;
}

void KeyStore::touch (const std::string& id)
{
    std::lock_guard<std::mutex> lock (mutex_);
    lastUsed_[id] = now();
}

bool KeyStore::inUse()
{
    std::lock_guard<std::mutex> lock (mutex_);
    refresh (false);
    return broken_ || ! records_.empty();
}

} // namespace rovox
