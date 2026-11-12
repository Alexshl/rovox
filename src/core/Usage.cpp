// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Usage.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>

#include <fcntl.h>
#include <unistd.h>

namespace rovox
{

double systemNowS()
{
    return std::chrono::duration<double> (std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string isoUtc (double unixS)
{
    const std::time_t t = (std::time_t) std::floor (unixS);
    std::tm tm {};
    ::gmtime_r (&t, &tm);
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::optional<double> parseIsoUtc (const std::string& s)
{
    std::tm tm {};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    int n = std::sscanf (s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);

    if (n != 6)
    {
        h = mi = sec = 0;
        n = std::sscanf (s.c_str(), "%d-%d-%d", &y, &mo, &d);

        if (n != 3)
            return std::nullopt;
    }

    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60)
        return std::nullopt;

    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    return (double) ::timegm (&tm);
}

double roundUsage (double v)
{
    return std::round (v * 1000.0) / 1000.0;
}

//==============================================================================
json UsageRecord::toJson() const
{
    return { { "ts", isoUtc (tsS) }, { "tenant", tenant }, { "key_id", keyId }, { "job_id", jobId }, { "kind", kind }, { "cpu_s", cpuS }, { "audio_s", audioS },
             { "bytes", bytes }, { "outputs", outputs }, { "external_ref", externalRef } };
}

std::optional<UsageRecord> UsageRecord::fromJson (const json& j)
{
    if (! j.is_object() || ! j.contains ("ts") || ! j["ts"].is_string() || ! j.contains ("tenant") || ! j["tenant"].is_string())
        return std::nullopt;

    const auto ts = parseIsoUtc (j["ts"].get<std::string>());

    if (! ts)
        return std::nullopt;

    UsageRecord r;
    r.tsS = *ts;
    r.tenant = j["tenant"].get<std::string>();
    r.keyId = j.value ("key_id", "");
    r.jobId = j.value ("job_id", "");
    r.kind = j.value ("kind", "");
    r.cpuS = j.value ("cpu_s", 0.0);
    r.audioS = j.value ("audio_s", 0.0);
    r.bytes = j.value ("bytes", (std::int64_t) 0);
    r.outputs = j.contains ("outputs") && j["outputs"].is_array() ? j["outputs"] : json::array();
    r.externalRef = j.value ("external_ref", "");
    return r;
}

//==============================================================================
UsageJournal::UsageJournal (fs::path dir, NowFn now) : dir_ (std::move (dir)), now_ (now ? std::move (now) : NowFn (systemNowS)) {}

fs::path UsageJournal::fileFor (double unixS) const
{
    return dir_ / ("usage-" + isoUtc (unixS).substr (0, 7) + ".jsonl");
}

Result<void> UsageJournal::append (UsageRecord& r)
{
    if (r.tsS <= 0)
        r.tsS = now_();

    const auto line = r.toJson().dump() + "\n";
    std::lock_guard lock (mutex_);
    std::error_code ec;
    fs::create_directories (dir_, ec);
    const int fd = ::open (fileFor (r.tsS).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    if (fd < 0)
        return makeError (errc::io, "cannot open the usage journal in " + dir_.string());

    const char* p = line.data();
    size_t left = line.size();
    bool ok = true;

    while (left > 0)
    {
        const auto w = ::write (fd, p, left);

        if (w < 0 && errno == EINTR)
            continue;

        if (w <= 0)
        {
            ok = false;
            break;
        }

        p += w;
        left -= (size_t) w;
    }

    ::close (fd);

    if (! ok)
        return makeError (errc::io, "cannot write the usage journal in " + dir_.string());

    return {};
}

std::vector<UsageRecord> UsageJournal::read (std::optional<std::string> tenant, std::optional<double> fromS, std::optional<double> toS) const
{
    std::vector<UsageRecord> out;
    std::vector<fs::path> files;
    {
        std::lock_guard lock (mutex_);
        std::error_code ec;

        for (fs::directory_iterator it (dir_, ec), end; ! ec && it != end; it.increment (ec))
        {
            const auto name = it->path().filename().string();

            if (name.rfind ("usage-", 0) != 0 || it->path().extension() != ".jsonl")
                continue;

            // usage-YYYY-MM.jsonl: skip the months that cannot overlap [from, to)
            const auto monthStart = parseIsoUtc (name.substr (6, 7) + "-01");

            if (monthStart && ((toS && *monthStart >= *toS) || (fromS && *monthStart + 32 * 86400.0 <= *fromS)))
                continue;

            files.push_back (it->path());
        }
    }

    std::sort (files.begin(), files.end());

    for (auto& f : files)
    {
        std::ifstream in (f);
        std::string line;

        while (std::getline (in, line))
        {
            json j = json::parse (line, nullptr, false);
            auto r = UsageRecord::fromJson (j);

            if (! r || (tenant && r->tenant != *tenant) || (fromS && r->tsS < *fromS) || (toS && r->tsS >= *toS))
                continue;

            out.push_back (std::move (*r));
        }
    }

    return out;
}

json UsageJournal::report (const UsageQuery& q) const
{
    struct Sum { std::int64_t jobs = 0, bytes = 0; double cpuS = 0, audioS = 0; };
    std::map<std::string, Sum> groups;
    Sum total;

    for (auto& r : read (q.tenant, q.fromS, q.toS))
    {
        std::string key = r.tenant;

        if (q.groupBy == "key")         key = r.keyId.empty() ? "(none)" : r.keyId;
        else if (q.groupBy == "kind")   key = r.kind;
        else if (q.groupBy == "day")    key = isoUtc (r.tsS).substr (0, 10);

        for (auto* s : { &groups[key], &total })
        {
            ++s->jobs;
            s->cpuS += r.cpuS;
            s->audioS += r.audioS;
            s->bytes += r.bytes;
        }
    }

    auto sumJson = [] (const Sum& s) { return json { { "jobs", s.jobs }, { "cpu_s", roundUsage (s.cpuS) }, { "audio_s", roundUsage (s.audioS) }, { "bytes", s.bytes } }; };
    json list = json::array();

    for (auto& [k, s] : groups)
    {
        json g = sumJson (s);
        g["key"] = k;
        list.push_back (g);
    }

    json out { { "group_by", q.groupBy }, { "groups", list }, { "total", sumJson (total) } };

    if (q.fromS)    out["from"] = isoUtc (*q.fromS);
    if (q.toS)      out["to"] = isoUtc (*q.toS);

    return out;
}

} // namespace rovox
