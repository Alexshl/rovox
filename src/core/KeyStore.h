// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// API keys in <data>/auth/keys.json (mode 0600, atomic writes). Only the SHA-256 of a key's secret is stored; the full key
// is returned once, by create / rotate. The file is re-read when its mtime or size changes (checked at most once a second),
// so an edit by the CLI or by hand takes effect in about a second.
#pragma once

#include "Auth.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace rovox
{

struct KeyRecord
{
    std::string id, label, tenant;
    ScopeSet scopes;
    std::string quotaProfile;                   // empty = the tenant's default
    std::int64_t createdAt = 0;                 // Unix seconds
    std::optional<std::int64_t> expiresAt, revokedAt;
    std::int64_t lastUsedAt = 0;                // 0 = never
    std::string hash;                           // "sha256:<hex>"

    bool revoked() const                        { return revokedAt.has_value(); }
    bool expired (std::int64_t now) const       { return expiresAt.has_value() && now >= *expiresAt; }
    /** Everything except the hash. */
    json toPublicJson() const;
};

class KeyStore
{
public:
    struct CreateSpec
    {
        std::string label, tenant = "local";
        ScopeSet scopes;
        std::string quotaProfile;
        std::optional<std::int64_t> expiresAt;
    };

    struct Created
    {
        json record;                            // public form (no hash)
        std::string key;                        // the full key, shown once
    };

    /** `clock` returns Unix seconds (tests inject one). The file need not exist. */
    explicit KeyStore (std::filesystem::path file, std::function<std::int64_t()> clock = {});

    /** INVALID_PARAMS for a bad tenant id, an empty scope set or `admin` for a tenant other than "local". */
    Result<Created> create (const CreateSpec&);
    /** Public records, never a hash or secret. */
    json list();
    Result<json> revoke (const std::string& id);
    /** New key with the same label, tenant, scopes and quota profile. The old key is revoked at once, or expires in `graceSeconds`. */
    Result<Created> rotate (const std::string& id, std::int64_t graceSeconds = 0);

    std::optional<KeyRecord> find (const std::string& id);
    /** Remembers use in memory (written to the file with the next change). */
    void touch (const std::string& id);
    /** True if the file holds any key (or could not be parsed): the server is then not "open". */
    bool inUse();

    std::int64_t now() const;
    const std::filesystem::path& file() const   { return file_; }

private:
    void refresh (bool force);                  // callers hold mutex_
    Result<void> save();                        // callers hold mutex_
    Result<Created> createLocked (const CreateSpec&);

    std::filesystem::path file_;
    std::function<std::int64_t()> clock_;
    std::mutex mutex_;
    std::map<std::string, KeyRecord> records_;
    std::map<std::string, std::int64_t> lastUsed_;
    bool broken_ = false;
    bool loaded_ = false;
    std::chrono::steady_clock::time_point lastCheck_;
    std::filesystem::file_time_type mtime_ {};
    std::uintmax_t size_ = 0;
    bool fileExisted_ = false;
};

} // namespace rovox
