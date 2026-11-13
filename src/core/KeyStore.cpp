// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "KeyStore.h"

#include "Ids.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
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

/** An exclusive flock on `<dir>/keys.lock` for the read-modify-write of keys.json across processes (the server and `rovox keys`). */
class KeyFileLock
{
public:
    explicit KeyFileLock (const fs::path& keysFile)
    {
        std::error_code ec;
        const auto dir = keysFile.parent_path();
        fs::create_directories (dir, ec);
        ::chmod (dir.c_str(), 0700);
        fd_ = ::open ((dir / "keys.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);

        if (fd_ >= 0)
            while (::flock (fd_, LOCK_EX) != 0 && errno == EINTR) {}
    }

    ~KeyFileLock()
    {
        if (fd_ >= 0)
        {
            ::flock (fd_, LOCK_UN);
            ::close (fd_);
        }
    }

    KeyFileLock (const KeyFileLock&) = delete;
    KeyFileLock& operator= (const KeyFileLock&) = delete;

private:
    int fd_ = -1;
};

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

std::string isoUtcS (std::int64_t t)
{
    const std::time_t tt = (std::time_t) t;
    std::tm tm {};
    ::gmtime_r (&tt, &tm);
    char buf[32];
    std::strftime (buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
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

Result<std::int64_t> parseExpiry (const std::string& text, std::int64_t nowS)
{
    const auto bad = makeError (errc::invalidParams, "expiry must be like 30d or an ISO date (2027-01-31, 2027-01-31T12:00:00Z)");

    if (text.size() >= 2 && text.back() == 'd' && std::all_of (text.begin(), text.end() - 1, [] (unsigned char c) { return std::isdigit (c) != 0; }))
    {
        if (text.size() > 6)
            return bad;

        const auto days = std::stoll (text.substr (0, text.size() - 1));

        if (days <= 0)
            return bad;

        return nowS + days * 86400;
    }

    std::tm tm {};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    char tail = 0;
    int n = std::sscanf (text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &sec, &tail);

    if (n == 3)
        h = mi = sec = 0;
    else if (! (n == 6 || (n == 7 && tail == 'Z' && text.back() == 'Z')))
        return bad;

    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60)
        return bad;

    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    const auto t = (std::int64_t) ::timegm (&tm);

    if (t <= nowS)
        return makeError (errc::invalidParams, "expiry is in the past");

    return t;
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

void KeyStore::audit (const std::string& action, const KeyRecord& r, const std::string& actor)
{
    const json line { { "ts", isoUtcS (now()) }, { "action", action }, { "key_id", r.id }, { "tenant", r.tenant }, { "label", r.label },
                      { "scopes", r.scopes.names() }, { "actor", actor } };
    const auto path = file_.parent_path() / "audit.jsonl";
    const int fd = ::open (path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);

    if (fd < 0)
        return;

    ::fchmod (fd, 0600);
    writeAll (fd, line.dump() + "\n");
    ::close (fd);
}

Result<KeyStore::Created> KeyStore::create (const CreateSpec& spec, const std::string& actor)
{
    std::lock_guard<std::mutex> lock (mutex_);
    KeyFileLock fileLock (file_);
    refresh (true);
    auto out = createLocked (spec);

    if (! out.ok())
        return out;

    if (auto saved = save(); ! saved.ok())
    {
        records_.erase (out.value().record["id"].get<std::string>());
        return saved.error();
    }

    audit ("create", records_[out.value().record["id"].get<std::string>()], actor);
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

Result<json> KeyStore::revoke (const std::string& id, const std::string& actor)
{
    std::lock_guard<std::mutex> lock (mutex_);
    KeyFileLock fileLock (file_);
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

    audit ("revoke", it->second, actor);
    return it->second.toPublicJson();
}

Result<KeyStore::Created> KeyStore::rotate (const std::string& id, std::int64_t graceSeconds, const std::string& actor)
{
    std::lock_guard<std::mutex> lock (mutex_);
    KeyFileLock fileLock (file_);
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

    audit ("rotate", records_[id], actor);
    audit ("create", records_[created.value().record["id"].get<std::string>()], actor);
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
