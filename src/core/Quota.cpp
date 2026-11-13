// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Quota.h"

#include "EventHub.h"

#include <toml++/toml.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace rovox
{

namespace
{
constexpr double storageCacheS = 60.0;

long dayOf (double t)       { return (long) std::floor (t / 86400.0); }

long monthOf (double t)
{
    const std::time_t s = (std::time_t) std::floor (t);
    std::tm tm {};
    ::gmtime_r (&s, &tm);
    return (long) (tm.tm_year + 1900) * 12 + tm.tm_mon;
}

double nextDayStart (double t)      { return (double) (dayOf (t) + 1) * 86400.0; }

double nextMonthStart (double t)
{
    const std::time_t s = (std::time_t) std::floor (t);
    std::tm tm {};
    ::gmtime_r (&s, &tm);
    tm.tm_mon += 1;
    tm.tm_mday = 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return (double) ::timegm (&tm);
}

double monthStart (double t)
{
    const std::time_t s = (std::time_t) std::floor (t);
    std::tm tm {};
    ::gmtime_r (&s, &tm);
    tm.tm_mday = 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return (double) ::timegm (&tm);
}

Error exceeded (const std::string& quota, double limit, double used, double resetsAt, const std::string& what)
{
    json d { { "quota", quota }, { "limit", limit }, { "used", roundUsage (used) } };

    if (resetsAt > 0)
        d["resets_at"] = isoUtc (resetsAt);

    return makeError (errc::quotaExceeded, what, d);
}

std::int64_t directorySize (const fs::path& dir)
{
    std::int64_t total = 0;
    std::error_code ec;

    for (fs::recursive_directory_iterator it (dir, fs::directory_options::skip_permission_denied, ec), end; ! ec && it != end; it.increment (ec))
    {
        std::error_code e2;

        if (it->is_regular_file (e2) && ! e2)
        {
            const auto n = it->file_size (e2);

            if (! e2)
                total += (std::int64_t) n;
        }
    }

    return total;
}

template <typename T>
void readNum (const toml::table& t, const char* key, std::optional<T>& out)
{
    if (auto v = t[key].template value<double>())
        out = (T) *v;
}
} // namespace

//==============================================================================
json QuotaProfile::toJson() const
{
    json j = json::object();

    if (requestsPerMin)         j["requests_per_min"] = *requestsPerMin;
    if (cpuSecondsPerDay)       j["cpu_seconds_per_day"] = *cpuSecondsPerDay;
    if (concurrentJobs)         j["concurrent_jobs"] = *concurrentJobs;
    if (projects)               j["projects"] = *projects;
    if (storageBytes)           j["storage_bytes"] = *storageBytes;
    if (audioMinutesPerMonth)   j["audio_minutes_per_month"] = *audioMinutesPerMonth;

    return j;
}

QuotaProfile QuotaConfig::builtInDefault()
{
    QuotaProfile p;
    p.requestsPerMin = 120;
    p.cpuSecondsPerDay = 3600;
    p.concurrentJobs = 4;
    p.projects = 50;
    p.storageBytes = (std::int64_t) 20 * 1024 * 1024 * 1024;
    return p;
}

QuotaConfig QuotaConfig::load (const fs::path& tomlFile)
{
    QuotaConfig c;
    std::error_code ec;

    if (tomlFile.empty() || ! fs::exists (tomlFile, ec))
        return c;

    try
    {
        const auto doc = toml::parse_file (tomlFile.string());

        if (auto* profiles = doc["quota_profiles"].as_table())
            for (auto&& [name, node] : *profiles)
                if (auto* t = node.as_table())
                {
                    QuotaProfile p;
                    readNum (*t, "requests_per_min", p.requestsPerMin);
                    readNum (*t, "cpu_seconds_per_day", p.cpuSecondsPerDay);
                    readNum (*t, "concurrent_jobs", p.concurrentJobs);
                    readNum (*t, "projects", p.projects);
                    readNum (*t, "storage_bytes", p.storageBytes);
                    readNum (*t, "audio_minutes_per_month", p.audioMinutesPerMonth);
                    c.profiles[std::string (name.str())] = p;
                }

        if (auto* tenants = doc["tenants"].as_table())
            for (auto&& [id, node] : *tenants)
                if (auto* t = node.as_table())
                {
                    if (auto name = (*t)["quota_profile"].value<std::string>())
                        c.tenantProfile[std::string (id.str())] = *name;

                    if (auto policy = (*t)["licence_policy"].value<std::string>())
                        c.tenantLicence[std::string (id.str())] = *policy;
                }
    }
    catch (const toml::parse_error&)
    {
        return QuotaConfig {};
    }

    return c;
}

QuotaProfile QuotaConfig::resolve (const std::string& tenant, const std::string& explicitProfile) const
{
    if (! explicitProfile.empty())
        if (auto it = profiles.find (explicitProfile); it != profiles.end())
            return it->second;

    if (auto t = tenantProfile.find (tenant); t != tenantProfile.end())
        if (auto it = profiles.find (t->second); it != profiles.end())
            return it->second;

    if (tenant == "local")
        return {};

    if (auto it = profiles.find ("default"); it != profiles.end())
        return it->second;

    return builtInDefault();
}

bool QuotaConfig::licenceRequiresServerUse (const std::string& tenant) const
{
    if (auto it = tenantLicence.find (tenant); it != tenantLicence.end())
        return it->second != "any";             // anything but an explicit "any" restricts

    return tenant != "local";
}

//==============================================================================
RateLimiter::RateLimiter (NowFn now) : now_ (now ? std::move (now) : NowFn (systemNowS)) {}

RateDecision RateLimiter::check (const std::string& keyId, const QuotaProfile& profile)
{
    if (! profile.requestsPerMin || *profile.requestsPerMin <= 0)
        return {};

    const double capacity = *profile.requestsPerMin, rate = capacity / 60.0, now = now_();
    std::lock_guard lock (mutex_);

    if (buckets_.size() > 4096)
        for (auto it = buckets_.begin(); it != buckets_.end();)
            it = now - it->second.last > 600.0 ? buckets_.erase (it) : std::next (it);

    auto [it, fresh] = buckets_.try_emplace (keyId);
    auto& b = it->second;

    if (fresh)
    {
        b.tokens = capacity;
        b.last = now;
    }

    b.capacity = capacity;
    b.tokens = std::min (capacity, b.tokens + std::max (0.0, now - b.last) * rate);
    b.last = now;

    if (b.tokens >= 1.0)
    {
        b.tokens -= 1.0;
        return {};
    }

    return { false, std::max (1.0, std::ceil ((1.0 - b.tokens) / rate)) };
}

//==============================================================================
QuotaTracker::QuotaTracker (NowFn now) : now_ (now ? std::move (now) : NowFn (systemNowS)) {}

void QuotaTracker::setStorageProvider (std::function<std::int64_t (const std::string&)> f)
{
    std::lock_guard lock (storageMutex_);
    storageProvider_ = std::move (f);
    storageCache_.clear();
}

QuotaTracker::Window& QuotaTracker::roll (const std::string& tenant, double now)
{
    auto& w = windows_[tenant];
    const long day = dayOf (now), month = monthOf (now);

    if (w.day != day)
    {
        w.day = day;
        w.cpuUsed = 0;
    }

    if (w.month != month)
    {
        w.month = month;
        w.audioUsed = 0;
    }

    return w;
}

double QuotaTracker::estimateCpu (const std::string& tenant, double audioS) const
{
    std::lock_guard lock (mutex_);
    auto it = windows_.find (tenant);
    return std::max (0.0, audioS) * (it == windows_.end() ? 1.0 : it->second.ratio);
}

Result<ReservationId> QuotaTracker::reserve (const std::string& tenant, const QuotaProfile& profile, double estimateCpuS, double estimateAudioS)
{
    const double now = now_();
    std::lock_guard lock (mutex_);
    auto& w = roll (tenant, now);

    if (profile.concurrentJobs && w.active >= *profile.concurrentJobs)
        return exceeded ("concurrent_jobs", *profile.concurrentJobs, w.active, 0, "too many jobs running for this tenant (limit " + std::to_string (*profile.concurrentJobs) + ")");

    if (profile.cpuSecondsPerDay)
    {
        const double limit = *profile.cpuSecondsPerDay;

        if (w.cpuUsed >= limit || w.cpuUsed + w.reservedCpu + estimateCpuS > limit)
            return exceeded ("cpu_seconds_per_day", limit, w.cpuUsed, nextDayStart (now), "the daily CPU quota of this tenant is used up");
    }

    if (profile.audioMinutesPerMonth)
    {
        const double limit = *profile.audioMinutesPerMonth * 60.0;

        if (w.audioUsed >= limit || w.audioUsed + estimateAudioS > limit)
            return exceeded ("audio_minutes_per_month", *profile.audioMinutesPerMonth, w.audioUsed / 60.0, nextMonthStart (now), "the monthly audio quota of this tenant is used up");
    }

    const ReservationId id = nextId_++;
    reservations_[id] = { tenant, std::max (0.0, estimateCpuS) };
    w.reservedCpu += std::max (0.0, estimateCpuS);
    ++w.active;
    return id;
}

void QuotaTracker::commit (ReservationId id, double actualCpuS, double audioS)
{
    const double now = now_();
    std::lock_guard lock (mutex_);
    auto r = reservations_.find (id);

    if (r == reservations_.end())
        return;

    auto& w = roll (r->second.tenant, now);
    w.reservedCpu = std::max (0.0, w.reservedCpu - r->second.cpuS);
    w.active = std::max (0, w.active - 1);
    w.cpuUsed += std::max (0.0, actualCpuS);
    w.audioUsed += std::max (0.0, audioS);

    if (audioS > 0.5)
        w.ratio = std::clamp (0.8 * w.ratio + 0.2 * (actualCpuS / audioS), 0.01, 50.0);

    reservations_.erase (r);
}

void QuotaTracker::release (ReservationId id)
{
    std::lock_guard lock (mutex_);
    auto r = reservations_.find (id);

    if (r == reservations_.end())
        return;

    auto& w = windows_[r->second.tenant];
    w.reservedCpu = std::max (0.0, w.reservedCpu - r->second.cpuS);
    w.active = std::max (0, w.active - 1);
    reservations_.erase (r);
}

Result<void> QuotaTracker::checkProjects (const std::string&, const QuotaProfile& profile, int currentCount)
{
    if (profile.projects && currentCount >= *profile.projects)
        return exceeded ("projects", *profile.projects, currentCount, 0, "this tenant has reached its project limit (" + std::to_string (*profile.projects) + ")");

    return {};
}

std::int64_t QuotaTracker::storageUsed (const std::string& tenant)
{
    std::function<std::int64_t (const std::string&)> provider;
    const double now = now_();
    {
        std::lock_guard lock (storageMutex_);
        auto it = storageCache_.find (tenant);

        if (it != storageCache_.end() && now - it->second.second < storageCacheS)
            return it->second.first;

        provider = storageProvider_;
    }

    if (! provider)
        return 0;

    const auto bytes = provider (tenant);       // directory walk: outside every lock
    std::lock_guard lock (storageMutex_);
    storageCache_[tenant] = { bytes, now };
    return bytes;
}

Result<void> QuotaTracker::checkStorage (const std::string& tenant, const QuotaProfile& profile, std::int64_t neededBytes)
{
    if (! profile.storageBytes)
        return {};

    const auto used = storageUsed (tenant);

    if (used >= *profile.storageBytes || used + std::max<std::int64_t> (0, neededBytes) > *profile.storageBytes)
        return exceeded ("storage_bytes", (double) *profile.storageBytes, (double) used, 0, "the storage quota of this tenant is used up");

    return {};
}

void QuotaTracker::addUsage (const std::string& tenant, double cpuS, double audioS, double atS)
{
    const double now = now_();
    std::lock_guard lock (mutex_);
    auto& w = roll (tenant, now);

    if (dayOf (atS) == dayOf (now))
        w.cpuUsed += cpuS;

    if (monthOf (atS) == monthOf (now))
        w.audioUsed += audioS;

    if (audioS > 0.5)
        w.ratio = std::clamp (0.8 * w.ratio + 0.2 * (cpuS / audioS), 0.01, 50.0);
}

json QuotaTracker::summary (const std::string& tenant, const QuotaProfile& profile)
{
    const double now = now_();
    json used;
    {
        std::lock_guard lock (mutex_);
        const auto& w = roll (tenant, now);
        used = { { "cpu_seconds_today", roundUsage (w.cpuUsed) }, { "cpu_seconds_reserved", roundUsage (w.reservedCpu) },
                 { "audio_minutes_month", roundUsage (w.audioUsed / 60.0) }, { "concurrent_jobs", w.active } };
    }

    if (profile.storageBytes)
        used["storage_bytes"] = storageUsed (tenant);

    return { { "limits", profile.toJson() }, { "used", used }, { "resets_at", isoUtc (nextDayStart (now)) }, { "month_resets_at", isoUtc (nextMonthStart (now)) } };
}

//==============================================================================
QuotaService::QuotaService (NowFn now) : now_ (now ? std::move (now) : NowFn (systemNowS)), tracker_ (now_), limiter_ (now_) {}

bool QuotaService::configured() const
{
    std::lock_guard lock (mutex_);
    return journal_ != nullptr;
}

void QuotaService::setConfigFile (fs::path file)
{
    std::lock_guard lock (mutex_);

    if (configFile_ != file)
    {
        configFile_ = std::move (file);
        configCheckedAt_ = 0;
        configSeen_ = false;
    }
}

void QuotaService::configure (const fs::path& dataDir, const fs::path& configFile)
{
    std::shared_ptr<UsageJournal> journal;
    {
        std::lock_guard lock (mutex_);

        if (! configFile.empty() && configFile_ != configFile)
        {
            configFile_ = configFile;
            configCheckedAt_ = 0;
            configSeen_ = false;
        }

        if (journal_ != nullptr && dataDir_ == dataDir)
            return;

        dataDir_ = dataDir;
        journal_ = journal = std::make_shared<UsageJournal> (dataDir / "usage", now_);
    }

    tracker_.setStorageProvider ([dataDir] (const std::string& tenant) { return directorySize (dataDir / "tenants" / tenant); });

    // the windows of today and of this month come from the journal
    for (auto& r : journal->read (std::nullopt, monthStart (now_()), std::nullopt))
        tracker_.addUsage (r.tenant, r.cpuS, r.audioS, r.tsS);
}

std::shared_ptr<const QuotaConfig> QuotaService::config()
{
    std::lock_guard lock (mutex_);
    const double now = now_();

    if (std::abs (now - configCheckedAt_) >= 1.0)
    {
        configCheckedAt_ = now;
        std::error_code ec;
        const auto mtime = configFile_.empty() ? fs::file_time_type {} : fs::last_write_time (configFile_, ec);
        const bool exists = ! configFile_.empty() && ! ec;

        if (exists && (! configSeen_ || mtime != configMtime_))
        {
            config_ = std::make_shared<const QuotaConfig> (QuotaConfig::load (configFile_));
            configMtime_ = mtime;
            configSeen_ = true;
        }
        else if (! exists && configSeen_)
        {
            config_ = std::make_shared<const QuotaConfig>();
            configSeen_ = false;
        }
    }

    return config_;
}

QuotaProfile QuotaService::profile (const std::string& tenant, const std::string& explicitProfile)
{
    return config()->resolve (tenant, explicitProfile);
}

bool QuotaService::licenceRequiresServerUse (const std::string& tenant)
{
    return config()->licenceRequiresServerUse (tenant);
}

RateDecision QuotaService::checkRate (const Principal& p)
{
    return limiter_.check (p.keyId, profile (p.tenant, p.quotaProfile));
}

Error QuotaService::rateLimitedError (const RateDecision& d)
{
    return makeError (errc::rateLimited, "too many requests; retry in " + std::to_string ((int) d.retryAfterS) + " s", json { { "retry_after_s", d.retryAfterS } });
}

Result<ReservationId> QuotaService::admit (const std::string& tenant, const std::string& explicitProfile, double audioS)
{
    return tracker_.reserve (tenant, profile (tenant, explicitProfile), tracker_.estimateCpu (tenant, audioS), audioS);
}

void QuotaService::finish (ReservationId id, UsageRecord rec, EventHub* hub)
{
    tracker_.commit (id, rec.cpuS, rec.audioS);
    std::shared_ptr<UsageJournal> journal;
    {
        std::lock_guard lock (mutex_);
        journal = journal_;
    }

    if (journal)
        journal->append (rec);
    else if (rec.tsS <= 0)
        rec.tsS = now_();

    if (hub != nullptr)
        hub->publish (rec.tenant, "", { { "type", "usage.recorded" }, { "record", rec.toJson() } });
}

void QuotaService::release (ReservationId id)
{
    tracker_.release (id);
}

Result<void> QuotaService::checkProjects (const std::string& tenant, const std::string& explicitProfile, int currentCount)
{
    return tracker_.checkProjects (tenant, profile (tenant, explicitProfile), currentCount);
}

Result<void> QuotaService::checkStorage (const std::string& tenant, std::int64_t neededBytes, const std::string& explicitProfile)
{
    return tracker_.checkStorage (tenant, profile (tenant, explicitProfile), neededBytes);
}

json QuotaService::summary (const std::string& tenant, const std::string& explicitProfile)
{
    auto j = tracker_.summary (tenant, profile (tenant, explicitProfile));
    j["tenant"] = tenant;
    return j;
}

Result<json> QuotaService::report (const json& params)
{
    UsageQuery q;
    const double now = now_();

    auto time = [&] (const char* key) -> Result<std::optional<double>>
    {
        if (! params.contains (key) || params[key].is_null())
            return std::optional<double> {};

        auto t = params[key].is_string() ? parseIsoUtc (params[key].get<std::string>()) : std::nullopt;

        if (! t)
            return makeError (errc::invalidParams, std::string (key) + " must be an ISO date (YYYY-MM-DD) or time (YYYY-MM-DDTHH:MM:SSZ)", json { { "path", std::string ("/") + key } });

        return std::optional<double> { *t };
    };

    auto from = time ("from");
    auto to = time ("to");

    if (! from.ok())    return from.error();
    if (! to.ok())      return to.error();

    q.fromS = from.value().value_or (monthStart (now));
    q.toS = to.value();

    // a date-only `to` includes that whole day
    if (q.toS && params["to"].get<std::string>().size() == 10)
        *q.toS += 86400.0;

    if (params.contains ("tenant") && params["tenant"].is_string())
        q.tenant = params["tenant"].get<std::string>();

    if (params.contains ("group_by") && params["group_by"].is_string())
        q.groupBy = params["group_by"].get<std::string>();

    if (q.groupBy != "tenant" && q.groupBy != "key" && q.groupBy != "kind" && q.groupBy != "day")
        return makeError (errc::invalidParams, "group_by must be tenant, key, kind or day", json { { "path", "/group_by" } });

    std::shared_ptr<UsageJournal> journal;
    {
        std::lock_guard lock (mutex_);
        journal = journal_;
    }

    if (! journal)
        journal = std::make_shared<UsageJournal> (fs::path(), now_);

    return journal->report (q);
}

QuotaService& quotas()
{
    static QuotaService service;
    return service;
}

} // namespace rovox
