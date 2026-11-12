// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E11-05: quota profiles, the per-key token bucket, the per-tenant CPU window, the usage journal and its rebuild after a restart, usage.report.
// Every clock is injected; nothing sleeps.
#include "EventHub.h"
#include "Quota.h"
#include "TestFramework.h"
#include "TestSupport.h"
#include "Usage.h"

#include <cmath>
#include <fstream>
#include <tuple>

using namespace rovox;

namespace
{
constexpr double day0 = 1700000000.0;       // 2023-11-14 22:13:20 UTC
constexpr double dayLength = 86400.0;

QuotaProfile cpuProfile (double cpuS, int jobs = 100)
{
    QuotaProfile p;
    p.cpuSecondsPerDay = cpuS;
    p.concurrentJobs = jobs;
    return p;
}

UsageRecord record (const std::string& tenant, const std::string& job, double cpuS, double audioS, const std::string& kind = "render", const std::string& key = "k_one")
{
    UsageRecord r;
    r.tenant = tenant;
    r.keyId = key;
    r.jobId = job;
    r.kind = kind;
    r.cpuS = cpuS;
    r.audioS = audioS;
    r.bytes = 1000;
    r.outputs = json::array ({ "renders/" + job + ".wav" });
    return r;
}
} // namespace

ROVOX_TEST (quota, token_bucket_refuses_the_121st_request_of_a_minute_and_leaves_other_keys_alone)
{
    double now = 5000.0;
    RateLimiter limiter ([&] { return now; });
    QuotaProfile p;
    p.requestsPerMin = 120;

    for (int i = 0; i < 100; ++i)
        CHECK (limiter.check ("k_a", p).allowed);

    for (int i = 0; i < 20; ++i)
        CHECK (limiter.check ("k_a", p).allowed);

    auto refused = limiter.check ("k_a", p);
    CHECK (! refused.allowed);
    CHECK (refused.retryAfterS >= 1.0);
    CHECK (refused.retryAfterS <= 2.0);

    CHECK (limiter.check ("k_b", p).allowed);               // another key is unaffected
    CHECK (limiter.check ("k_b", QuotaProfile {}).allowed); // no limit configured

    now += 0.4;
    CHECK (! limiter.check ("k_a", p).allowed);             // less than one token came back
    now += 0.2;
    CHECK (limiter.check ("k_a", p).allowed);               // 0.6 s at 2 tokens/s
    CHECK (! limiter.check ("k_a", p).allowed);

    now += 600.0;                                           // a long pause refills to the capacity, not beyond
    int allowed = 0;

    for (int i = 0; i < 200; ++i)
        allowed += limiter.check ("k_a", p).allowed ? 1 : 0;

    CHECK (allowed == 120);
}

ROVOX_TEST (quota, profiles_come_from_the_config_file_and_local_is_unlimited)
{
    const auto file = test::makeTempDir ("quota-config") / "rovox.toml";
    {
        std::ofstream out (file);
        out << "[quota_profiles.pro]\nrequests_per_min = 600\ncpu_seconds_per_day = 86400\nconcurrent_jobs = 8\nprojects = 500\nstorage_bytes = 1000000\naudio_minutes_per_month = 6000\n\n"
               "[quota_profiles.free]\ncpu_seconds_per_day = 60\n\n"
               "[tenants.acme]\nquota_profile = \"pro\"\n";
    }

    const auto cfg = QuotaConfig::load (file);
    const auto pro = cfg.resolve ("acme");
    CHECK ((pro.requestsPerMin == 600.0));
    CHECK ((pro.cpuSecondsPerDay == 86400.0));
    CHECK ((pro.concurrentJobs == 8));
    CHECK ((pro.projects == 500));
    CHECK ((pro.storageBytes == 1000000));
    CHECK ((pro.audioMinutesPerMonth == 6000.0));

    const auto free = cfg.resolve ("someone", "free");      // a key's own profile
    CHECK ((free.cpuSecondsPerDay == 60.0));
    CHECK (! free.requestsPerMin.has_value());              // missing = unlimited

    const auto def = cfg.resolve ("nobody");                // built-in default for any non-local tenant
    CHECK ((def.requestsPerMin == 120.0));
    CHECK ((def.cpuSecondsPerDay == 3600.0));
    CHECK ((def.concurrentJobs == 4));
    CHECK ((def.projects == 50));
    CHECK ((def.storageBytes == (std::int64_t) 20 * 1024 * 1024 * 1024));

    const auto local = cfg.resolve ("local");
    CHECK (local.toJson().empty());                         // unlimited

    CHECK (QuotaConfig::load (file.parent_path() / "missing.toml").resolve ("local").toJson().empty());
    CHECK ((QuotaConfig::load (file.parent_path() / "missing.toml").resolve ("x").projects == 50));
}

ROVOX_TEST (quota, cpu_window_reserve_commit_release_exceed_and_new_utc_day)
{
    double now = day0;
    QuotaTracker tracker ([&] { return now; });
    const auto p = cpuProfile (100);

    auto a = tracker.reserve ("qt-cpu", p, 40);
    REQUIRE (a.ok());
    tracker.commit (a.value(), 30, 10);

    auto tooBig = tracker.reserve ("qt-cpu", p, 80);        // 30 used + 80 > 100
    REQUIRE (! tooBig.ok());
    CHECK (tooBig.error().code == errc::quotaExceeded);
    CHECK (tooBig.error().details["limit"] == 100.0);
    CHECK (tooBig.error().details["used"] == 30.0);
    CHECK (tooBig.error().details["resets_at"] == isoUtc ((std::floor (day0 / dayLength) + 1) * dayLength));

    auto b = tracker.reserve ("qt-cpu", p, 60);
    REQUIRE (b.ok());
    CHECK (! tracker.reserve ("qt-cpu", p, 20).ok());       // 30 + 60 reserved + 20 > 100
    tracker.release (b.value());
    CHECK (tracker.reserve ("qt-cpu", p, 20).ok());         // the released estimate is free again

    auto c = tracker.reserve ("qt-cpu2", p, 70);            // another tenant has its own window
    REQUIRE (c.ok());
    tracker.commit (c.value(), 105, 50);                    // actual may exceed the estimate
    auto exhausted = tracker.reserve ("qt-cpu2", p, 0);
    REQUIRE (! exhausted.ok());                             // used >= limit refuses even a zero estimate
    CHECK (exhausted.error().code == errc::quotaExceeded);
    CHECK (exhausted.error().details["used"] == 105.0);

    now = (std::floor (day0 / dayLength) + 1) * dayLength + 1.0;     // the next UTC day
    CHECK (tracker.reserve ("qt-cpu2", p, 50).ok());
    CHECK (tracker.summary ("qt-cpu2", p)["used"]["cpu_seconds_today"] == 0.0);
}

ROVOX_TEST (quota, concurrent_jobs_limit_and_local_unlimited)
{
    QuotaTracker tracker;
    QuotaProfile two;
    two.concurrentJobs = 2;

    auto a = tracker.reserve ("qt-conc", two, 0);
    auto b = tracker.reserve ("qt-conc", two, 0);
    REQUIRE (a.ok());
    REQUIRE (b.ok());
    auto c = tracker.reserve ("qt-conc", two, 0);
    REQUIRE (! c.ok());
    CHECK (c.error().code == errc::quotaExceeded);
    CHECK (c.error().details["quota"] == "concurrent_jobs");
    CHECK (tracker.reserve ("qt-conc-other", two, 0).ok());
    tracker.release (a.value());
    CHECK (tracker.reserve ("qt-conc", two, 0).ok());
    tracker.commit (b.value(), 1, 1);

    // tenant local has no limits whatever is asked
    const auto local = QuotaConfig {}.resolve ("local");

    for (int i = 0; i < 50; ++i)
        CHECK (tracker.reserve ("local", local, 1.0e9).ok());

    // the project count and the audio minutes are limits of their own
    QuotaProfile few;
    few.projects = 3;
    few.audioMinutesPerMonth = 1;
    CHECK (tracker.checkProjects ("qt-conc", few, 2).ok());
    CHECK (tracker.checkProjects ("qt-conc", few, 3).error().code == errc::quotaExceeded);
    CHECK (tracker.reserve ("qt-audio", few, 0, 30).ok());
    CHECK (! tracker.reserve ("qt-audio", few, 0, 61).ok());
}

ROVOX_TEST (quota, storage_size_is_cached_for_a_minute)
{
    double now = day0;
    QuotaTracker tracker ([&] { return now; });
    int calls = 0;
    std::int64_t size = 900;
    tracker.setStorageProvider ([&] (const std::string&) { ++calls; return size; });
    QuotaProfile p;
    p.storageBytes = 1000;

    CHECK (tracker.checkStorage ("qt-store", p, 50).ok());
    auto over = tracker.checkStorage ("qt-store", p, 200);
    REQUIRE (! over.ok());
    CHECK (over.error().code == errc::quotaExceeded);
    CHECK (calls == 1);

    size = 2000;
    now += 30;
    CHECK (tracker.checkStorage ("qt-store", p, 0).ok());   // still the cached 900
    CHECK (calls == 1);
    now += 31;
    CHECK (! tracker.checkStorage ("qt-store", p, 0).ok());
    CHECK (calls == 2);
    CHECK (tracker.checkStorage ("qt-store", QuotaProfile {}, (std::int64_t) 1000000000000).ok());
}

ROVOX_TEST (quota, journal_is_rebuilt_after_a_restart_and_report_matches_the_job_usage)
{
    const auto data = test::makeTempDir ("quota-journal");
    const double now = day0;
    EventHub hub;
    auto sub = hub.subscribe ({ .tenant = "qt-jr" });

    double sumCpu = 0, sumAudio = 0;
    {
        QuotaService svc ([&] { return now; });
        svc.configure (data, {}, &hub);
        CHECK (svc.configured());

        const std::vector<std::tuple<std::string, double, double, std::string>> jobs {
            { "job1", 12.345, 30.0, "render" }, { "job2", 3.2105, 10.5, "preview" }, { "job3", 40.001, 120.25, "render" }, { "job4", 0.5, 5.0, "analysis" } };

        for (auto& [id, cpuS, audioS, kind] : jobs)
        {
            auto r = svc.admit ("qt-jr", "", audioS);
            REQUIRE (r.ok());
            svc.finish (r.value(), record ("qt-jr", id, roundUsage (cpuS), roundUsage (audioS), kind));
            sumCpu += roundUsage (cpuS);
            sumAudio += roundUsage (audioS);
        }

        auto other = svc.admit ("qt-jr-other", "", 1.0);
        REQUIRE (other.ok());
        svc.finish (other.value(), record ("qt-jr-other", "jobx", 99.0, 1.0));
    }

    Event e;
    REQUIRE (sub->tryPop (e));
    CHECK (e.body["type"] == "usage.recorded");
    CHECK (e.body["record"]["job_id"] == "job1");
    CHECK (e.tenant == "qt-jr");

    // the journal is a JSONL file per month, one line per record
    const auto file = data / "usage" / "usage-2023-11.jsonl";
    REQUIRE (fs::exists (file));
    int lines = 0;
    {
        std::ifstream in (file);
        std::string line;

        while (std::getline (in, line))
        {
            auto j = json::parse (line);
            CHECK (j.contains ("ts"));
            CHECK (j.contains ("outputs"));
            ++lines;
        }
    }
    CHECK (lines == 5);

    // "restart": new objects on the same directory
    QuotaService restarted ([&] { return now; });
    restarted.configure (data, {}, nullptr);
    const auto summary = restarted.summary ("qt-jr");
    CHECK_NEAR (summary["used"]["cpu_seconds_today"].get<double>(), sumCpu, 0.01);
    CHECK_NEAR (summary["used"]["audio_minutes_month"].get<double>(), sumAudio / 60.0, 0.001);
    CHECK (summary["limits"]["cpu_seconds_per_day"] == 3600.0);
    CHECK (summary["used"]["concurrent_jobs"].get<int>() == 0);

    auto report = restarted.report ({ { "tenant", "qt-jr" }, { "from", "2023-11-01" }, { "to", "2023-11-30" } });
    REQUIRE (report.ok());
    CHECK_NEAR (report.value()["total"]["cpu_s"].get<double>(), sumCpu, sumCpu * 0.01);
    CHECK_NEAR (report.value()["total"]["audio_s"].get<double>(), sumAudio, sumAudio * 0.01);
    CHECK (report.value()["total"]["jobs"] == 4);
    CHECK (report.value()["groups"].size() == 1);

    auto byKind = restarted.report ({ { "tenant", "qt-jr" }, { "from", "2023-11-01" }, { "group_by", "kind" } });
    REQUIRE (byKind.ok());
    CHECK (byKind.value()["groups"].size() == 3);

    auto byDay = restarted.report ({ { "from", "2023-11-14" }, { "to", "2023-11-14" }, { "group_by", "day" } });
    REQUIRE (byDay.ok());
    CHECK (byDay.value()["groups"][0]["key"] == "2023-11-14");
    CHECK (byDay.value()["total"]["jobs"] == 5);

    auto empty = restarted.report ({ { "from", "2023-12-01" } });
    REQUIRE (empty.ok());
    CHECK (empty.value()["total"]["jobs"].get<int>() == 0);

    CHECK (restarted.report ({ { "from", "yesterday" } }).error().code == errc::invalidParams);
    CHECK (restarted.report ({ { "group_by", "colour" } }).error().code == errc::invalidParams);

    // the window of another day is not carried over: a later restart on the next UTC day starts at zero, the month keeps counting
    const double tomorrow = (std::floor (day0 / dayLength) + 1) * dayLength + 60.0;
    QuotaService nextDay ([&] { return tomorrow; });
    nextDay.configure (data, {}, nullptr);
    CHECK (nextDay.summary ("qt-jr")["used"]["cpu_seconds_today"] == 0.0);
    CHECK (nextDay.summary ("qt-jr")["used"]["audio_minutes_month"].get<double>() > 0.0);
}

ROVOX_TEST (quota, service_rate_check_uses_the_principals_key_and_profile)
{
    double now = day0;
    QuotaService svc ([&] { return now; });
    Principal acme;
    acme.tenant = "qt-rate";
    acme.keyId = "k_rate1";
    Principal other = acme;
    other.keyId = "k_rate2";

    for (int i = 0; i < 120; ++i)
        CHECK (svc.checkRate (acme).allowed);

    auto refused = svc.checkRate (acme);
    CHECK (! refused.allowed);
    CHECK (QuotaService::rateLimitedError (refused).code == errc::rateLimited);
    CHECK (QuotaService::rateLimitedError (refused).details["retry_after_s"].get<double>() >= 1.0);
    CHECK (svc.checkRate (other).allowed);

    for (int i = 0; i < 500; ++i)
        CHECK (svc.checkRate (Principal::local()).allowed);
}
