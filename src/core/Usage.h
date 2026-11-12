// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The usage journal: an append-only JSONL file per month, `<data>/usage/usage-YYYY-MM.jsonl`, one record per finished metered job.
// The record is the single source of truth for what a job cost: the job result's `usage` object, the quota windows (rebuilt from the
// journal at startup) and `usage.report` all read the same numbers.
#pragma once

#include "Result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rovox
{

namespace fs = std::filesystem;

/** Unix seconds, injectable for tests. */
using NowFn = std::function<double()>;

double systemNowS();
/** "YYYY-MM-DDTHH:MM:SSZ" */
std::string isoUtc (double unixS);
/** Accepts "YYYY-MM-DDTHH:MM:SSZ" and "YYYY-MM-DD" (midnight UTC). */
std::optional<double> parseIsoUtc (const std::string&);
/** Rounds to milliseconds: the value the job result and the journal both carry. */
double roundUsage (double v);

struct UsageRecord
{
    double tsS = 0;                 // unix seconds; written as "ts" in ISO form
    std::string tenant, keyId, jobId, kind;
    double cpuS = 0, audioS = 0;
    std::int64_t bytes = 0;
    json outputs = json::array();
    std::string externalRef;

    json toJson() const;
    static std::optional<UsageRecord> fromJson (const json&);
};

struct UsageQuery
{
    std::optional<std::string> tenant;
    std::optional<double> fromS, toS;       // [from, to)
    std::string groupBy = "tenant";         // tenant | key | kind | day
};

class UsageJournal
{
public:
    /** `dir` is `<data>/usage`; it is created on the first append. */
    explicit UsageJournal (fs::path dir, NowFn now = systemNowS);

    /** One write(2) per record (flushed at once, no buffering, no fsync). Stamps `tsS` with the clock if it is 0. */
    Result<void> append (UsageRecord&);

    /** Records of the files that can overlap [from, to), in file order, filtered by tenant and time. */
    std::vector<UsageRecord> read (std::optional<std::string> tenant, std::optional<double> fromS, std::optional<double> toS) const;

    /** {from, to, group_by, groups: [{key, jobs, cpu_s, audio_s, bytes}], total: {...}}. */
    json report (const UsageQuery&) const;

    const fs::path& dir() const     { return dir_; }

private:
    fs::path fileFor (double unixS) const;

    fs::path dir_;
    NowFn now_;
    mutable std::mutex mutex_;
};

} // namespace rovox
