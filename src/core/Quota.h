// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Quotas and rate limits per tenant and key. Profiles come from the configuration file:
//   [quota_profiles.<name>]  requests_per_min, cpu_seconds_per_day, concurrent_jobs, projects, storage_bytes, audio_minutes_per_month
//   [tenants.<id>]           quota_profile = "<name>", licence_policy = "any" | "server_use_ok"
// A missing limit is unlimited. Tenant `local` is unlimited unless configured; any other tenant without an explicit profile gets the profile
// named "default" (built in: 120 requests/min, 3600 CPU-seconds/day, 4 concurrent jobs, 50 projects, 20 GiB; the file may override it).
// CPU accounting of in-process renders is approximate (CLOCK_THREAD_CPUTIME_ID of the job's worker thread); child renders are exact (rusage).
#pragma once

#include "Auth.h"
#include "Result.h"
#include "Usage.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace rovox
{

class EventHub;

struct QuotaProfile
{
    std::optional<double> requestsPerMin, cpuSecondsPerDay, audioMinutesPerMonth;
    std::optional<int> concurrentJobs, projects;
    std::optional<std::int64_t> storageBytes;

    json toJson() const;       // only the limits that are set
};

struct QuotaConfig
{
    std::map<std::string, QuotaProfile> profiles;
    std::map<std::string, std::string> tenantProfile;
    std::map<std::string, std::string> tenantLicence;        // [tenants.<id>] licence_policy, as written

    static QuotaConfig load (const fs::path& tomlFile);         // missing or unparsable file = empty
    static QuotaProfile builtInDefault();

    /** `explicitProfile` (a key's own profile) wins, then [tenants.<id>], then local = unlimited, then "default". An unknown name falls through. */
    QuotaProfile resolve (const std::string& tenant, const std::string& explicitProfile = {}) const;

    /** True when the tenant may only use plugins with server_use_ok: [tenants.<id>] licence_policy = "server_use_ok". An explicit "any" lifts it;
        without a record `local` is unrestricted and every other tenant is restricted (fail closed). */
    bool licenceRequiresServerUse (const std::string& tenant) const;
};

//==============================================================================
struct RateDecision
{
    bool allowed = true;
    double retryAfterS = 0;         // whole seconds >= 1 when refused
};

/** In-memory token bucket per key id: capacity = requests_per_min, refilled continuously. Other keys are never affected. */
class RateLimiter
{
public:
    explicit RateLimiter (NowFn now = systemNowS);
    RateDecision check (const std::string& keyId, const QuotaProfile&);

private:
    struct Bucket { double tokens = 0, capacity = 0, last = 0; };

    NowFn now_;
    std::mutex mutex_;
    std::map<std::string, Bucket> buckets_;
};

//==============================================================================
using ReservationId = std::uint64_t;

/** Per-tenant windows: CPU seconds per UTC day, audio seconds per UTC month, running jobs, plus cached storage size. Thread-safe; the storage
    provider runs outside the lock. */
class QuotaTracker
{
public:
    explicit QuotaTracker (NowFn now = systemNowS);

    /** Directory size of a tenant in bytes; called at most once per 60 s and tenant, never under the lock. */
    void setStorageProvider (std::function<std::int64_t (const std::string& tenant)>);

    /** Admission of a job: running jobs, CPU of today (used >= limit or used + reserved + estimate > limit) and audio of the month.
        QUOTA_EXCEEDED details: {quota, limit, used, resets_at}. On success the job holds a slot and `estimateCpuS` until commit/release. */
    Result<ReservationId> reserve (const std::string& tenant, const QuotaProfile&, double estimateCpuS, double estimateAudioS = 0);
    void commit (ReservationId, double actualCpuS, double audioS);
    void release (ReservationId);

    /** The learned CPU-seconds per audio-second of the tenant (EMA of actual/audio, default 1.0). */
    double estimateCpu (const std::string& tenant, double audioS) const;

    Result<void> checkProjects (const std::string& tenant, const QuotaProfile&, int currentCount);
    Result<void> checkStorage (const std::string& tenant, const QuotaProfile&, std::int64_t neededBytes);

    /** Replays a journal record into the current windows (startup); records of other days/months only feed the learned ratio. */
    void addUsage (const std::string& tenant, double cpuS, double audioS, double atS);

    json summary (const std::string& tenant, const QuotaProfile&);

private:
    struct Window
    {
        long day = -1, month = -1;
        double cpuUsed = 0, audioUsed = 0, reservedCpu = 0;
        int active = 0;
        double ratio = 1.0;
    };

    struct Reservation { std::string tenant; double cpuS = 0; };

    Window& roll (const std::string& tenant, double now);
    std::int64_t storageUsed (const std::string& tenant);

    NowFn now_;
    mutable std::mutex mutex_;
    std::map<std::string, Window> windows_;
    std::map<ReservationId, Reservation> reservations_;
    ReservationId nextId_ = 1;

    std::mutex storageMutex_;
    std::function<std::int64_t (const std::string&)> storageProvider_;
    std::map<std::string, std::pair<std::int64_t, double>> storageCache_;     // tenant -> bytes, computed at
};

//==============================================================================
/** Everything above wired together: configuration (reloaded when the file's mtime changes), limiter, tracker and the usage journal.
    One process-wide instance, see quotas(). Not configured = no journal and the built-in default profile; `local` is unlimited either way. */
class QuotaService
{
public:
    explicit QuotaService (NowFn now = systemNowS);

    /** Sets the data directory (journal in <data>/usage, storage size of <data>/tenants/<t>) and the configuration file; rebuilds the windows
        from the journal. Calling it again with the same data dir only updates the file (if not empty). The service is process-wide and outlives
        any one server, so it keeps no hub: finish() is handed the hub of the job's owner. */
    void configure (const fs::path& dataDir, const fs::path& configFile);
    void setConfigFile (fs::path file);
    bool configured() const;

    QuotaProfile profile (const std::string& tenant, const std::string& explicitProfile = {});
    /** The licence policy of the tenant, see QuotaConfig::licenceRequiresServerUse. */
    bool licenceRequiresServerUse (const std::string& tenant);

    /** Per request (HTTP, each WS command, each MCP POST): the token bucket of the principal's key. */
    RateDecision checkRate (const Principal&);
    static Error rateLimitedError (const RateDecision&);

    /** Admission of a job of `audioS` audio seconds (CPU estimate = audioS x learned ratio). QUOTA_EXCEEDED creates nothing. */
    Result<ReservationId> admit (const std::string& tenant, const std::string& explicitProfile, double audioS);
    /** Closes the reservation with the measured numbers, appends the journal record and publishes `usage.recorded` for the tenant. */
    /** Commits the usage, appends it to the journal and publishes `usage.recorded` on `hub` (when given). */
    void finish (ReservationId, UsageRecord, EventHub* hub = nullptr);
    void release (ReservationId);

    Result<void> checkProjects (const std::string& tenant, const std::string& explicitProfile, int currentCount);
    /** Refuses when the tenant's cached storage size + neededBytes exceeds its limit. Call it before accepting bytes (upload create: HttpServer). */
    Result<void> checkStorage (const std::string& tenant, std::int64_t neededBytes, const std::string& explicitProfile = {});

    /** usage.summary: {tenant, limits, used {...}, resets_at}. */
    json summary (const std::string& tenant, const std::string& explicitProfile = {});
    /** usage.report: params {from?, to?, tenant?, group_by?}; from/to are ISO date or time, default the current month. */
    Result<json> report (const json& params);

    QuotaTracker& tracker()     { return tracker_; }
    RateLimiter& limiter()      { return limiter_; }

private:
    std::shared_ptr<const QuotaConfig> config();

    NowFn now_;
    QuotaTracker tracker_;
    RateLimiter limiter_;

    mutable std::mutex mutex_;
    fs::path dataDir_, configFile_;
    std::shared_ptr<UsageJournal> journal_;
    std::shared_ptr<const QuotaConfig> config_ = std::make_shared<const QuotaConfig>();
    fs::file_time_type configMtime_ {};
    bool configSeen_ = false;
    double configCheckedAt_ = 0;
};

/** The process-wide service. */
QuotaService& quotas();

} // namespace rovox
