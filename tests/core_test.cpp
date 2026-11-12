// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Url.h"
#include "TestSupport.h"
#include "Subprocess.h"
#include "JobManager.h"
#include "Cgroup.h"
#include "EventHub.h"
#include "Ids.h"
#include "RequestGuard.h"
#include "Spectrum.h"
#include "TestFramework.h"

#include <mutex>
#include <map>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <set>
#include <thread>
#include <vector>

using namespace rovox;

namespace
{
/** Deterministic pink noise at the given RMS: a fixed-seed xorshift white source through Paul Kellet's refined filter (+-0.05 dB from 9.2 Hz up at 44.1 kHz). */
std::vector<float> pinkNoise (double seconds, double sampleRate, double rmsDbfs)
{
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    auto white = [&]
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return (double) (state >> 11) / (double) (1ull << 52) - 1.0;        // [-1, 1)
    };

    const size_t n = (size_t) (seconds * sampleRate);
    std::vector<double> x (n);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum2 = 0;

    for (auto& v : x)
    {
        const double w = white();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        sum2 += v * v;
    }

    const double gain = std::pow (10.0, rmsDbfs / 20.0) / std::sqrt (sum2 / (double) n);
    std::vector<float> out (n);

    for (size_t i = 0; i < n; ++i)
        out[i] = (float) (x[i] * gain);

    return out;
}

/** Pink noise has power ~ 1/f, so a band holds power proportional to its width in octaves. Band offsets relative to lowmid_250_2k, in dB, from the band edges of Spectrum.cpp. */
double pinkOffsetDb (double lo, double hi)
{
    return 10.0 * std::log10 (std::log2 (hi / lo) / std::log2 (2000.0 / 250.0));
}
}

ROVOX_TEST (core, ulid_sorted_and_unique)
{
    std::set<std::string> seen;
    std::string previous;

    for (int i = 0; i < 20000; ++i)
    {
        auto id = newId ("trk");
        CHECK (isValidId (id, "trk"));
        CHECK (id > previous);
        previous = id;
        seen.insert (id);
    }

    CHECK (seen.size() == 20000);
    CHECK (! isValidId ("trk_short", "trk"));
    CHECK (! isValidId (newId ("itm"), "trk"));
}

ROVOX_TEST (core, tenant_ids)
{
    CHECK (isValidTenantId ("local"));
    CHECK (isValidTenantId ("a_b-9"));
    CHECK (! isValidTenantId ("../x"));
    CHECK (! isValidTenantId ("A"));
    CHECK (! isValidTenantId (""));
    CHECK (! isValidTenantId (std::string (65, 'a')));
}

ROVOX_TEST (core, cgroup_cpu_max)
{
    CHECK_NEAR (parseCpuMax ("max 100000"), 0.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("200000 100000"), 2.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("150000 100000\n"), 1.5, 1e-9);
    CHECK_NEAR (parseCpuMax (""), 0.0, 1e-9);
    CHECK_NEAR (parseCpuMax ("garbage"), 0.0, 1e-9);
    CHECK (cpuBudget().cpus >= 1);
}

ROVOX_TEST (core, spectrum_of_pink_noise_follows_band_width_in_octaves)
{
    // 1/f noise: equal power per octave, so each band sits at 10*log10(octaves of the band / octaves of lowmid) relative to lowmid (not monotonically)
    constexpr double sr = 44100.0;
    const auto noise = pinkNoise (30.0, sr, -20.0);
    SpectrumAnalyser analyser (sr);
    analyser.add (noise.data(), (int) noise.size());
    const auto bands = analyser.finish();

    CHECK_NEAR (bands.sub - bands.lowMid, pinkOffsetDb (20, 60), 1.0);
    CHECK_NEAR (bands.low - bands.lowMid, pinkOffsetDb (60, 250), 1.0);
    CHECK_NEAR (bands.highMid - bands.lowMid, pinkOffsetDb (2000, 6000), 1.0);
    CHECK_NEAR (bands.high - bands.lowMid, pinkOffsetDb (6000, 20000), 1.0);
}

ROVOX_TEST (core, event_hub_observers_see_strictly_increasing_ids_from_parallel_publishers)
{
    EventHub hub;
    std::vector<std::uint64_t> seen;        // only the observer touches it, and publishes never run concurrently
    const int id = hub.addObserver ([&] (const Event& e) { seen.push_back (e.id); });
    constexpr int perThread = 2000;

    auto work = [&]
    {
        for (int i = 0; i < perThread; ++i)
            hub.publish ("local", "", json { { "type", "test" } });
    };

    std::thread a (work), b (work);
    a.join();
    b.join();

    REQUIRE (seen.size() == 2 * perThread);

    for (size_t i = 1; i < seen.size(); ++i)
        CHECK (seen[i] > seen[i - 1]);

    hub.removeObserver (id);

    // removeObserver returns only when the observer is no longer running
    std::atomic<bool> inside { false }, finished { false };
    const int slow = hub.addObserver ([&] (const Event&) { inside = true; std::this_thread::sleep_for (std::chrono::milliseconds (100)); finished = true; });
    std::thread p ([&] { hub.publish ("local", "", json { { "type", "test" } }); });

    while (! inside)
        std::this_thread::yield();

    hub.removeObserver (slow);
    CHECK (finished.load());
    p.join();
}

//==============================================================================
// E10-20: subprocess pipes, process groups, output caps
namespace
{
double secondsSince (std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
}
} // namespace

ROVOX_TEST (core, subprocess_timeout_of_one_run_does_not_wait_for_a_concurrent_one)
{
    SubprocessResult slow;
    double slowSeconds = 0;

    std::thread a ([&]
    {
        const auto t0 = std::chrono::steady_clock::now();
        SubprocessOptions o;
        o.argv = { "/bin/sh", "-c", "sleep 30" };
        o.timeoutS = 0.3;
        slow = runSubprocess (o);
        slowSeconds = secondsSince (t0);
    });

    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    SubprocessResult other;
    std::thread b ([&]
    {
        SubprocessOptions o;
        o.argv = { "/bin/sh", "-c", "sleep 3; echo done" };
        other = runSubprocess (o);
    });

    a.join();
    CHECK (slow.timedOut);
    CHECK (slowSeconds < 2.0);                  // not held up until the other child (3 s) closed the pipes it must not have inherited
    b.join();
    CHECK (other.succeeded());
    CHECK (other.stdoutText == "done\n");
}

ROVOX_TEST (core, subprocess_kill_takes_the_grandchildren_and_does_not_wait_for_their_pipes)
{
    const auto t0 = std::chrono::steady_clock::now();
    SubprocessOptions o;
    o.argv = { "/bin/sh", "-c", "sleep 30 & echo started; sleep 30" };      // the background sleep holds stdout open
    o.timeoutS = 0.3;
    const auto r = runSubprocess (o);
    CHECK (r.timedOut);
    CHECK (secondsSince (t0) < 3.0);
    CHECK (r.stdoutText.find ("started") != std::string::npos);
}

ROVOX_TEST (core, subprocess_output_is_capped)
{
    SubprocessOptions o;
    o.argv = { "/bin/sh", "-c", "head -c 20000000 /dev/zero | tr '\\0' a" };
    const auto r = runSubprocess (o);
    CHECK (r.succeeded());
    CHECK (r.stdoutText.size() < 17u * 1024 * 1024 + 64);
    CHECK (r.stdoutText.find ("[output truncated]") != std::string::npos);

    size_t longest = 0, lines = 0;
    SubprocessOptions l;
    l.argv = { "/bin/sh", "-c", "head -c 3000000 /dev/zero | tr '\\0' a" };       // one endless line
    l.onStdoutLine = [&] (const std::string& line) { longest = std::max (longest, line.size()); ++lines; };
    const auto lr = runSubprocess (l);
    CHECK (lr.succeeded());
    CHECK (longest <= 1024u * 1024 + 64);
    CHECK (lines >= 2);
}

//==============================================================================
// E10-25: the shared URL parser
ROVOX_TEST (core, url_parser_handles_ipv6_ports_and_defaults)
{
    auto u = parseUrl ("https://[2001:db8::1]:8443/x?y=1");
    REQUIRE (u.valid);
    CHECK (u.host == "2001:db8::1");
    CHECK (u.port == 8443);
    CHECK (u.path == "/x?y=1");
    CHECK (u.origin == "https://[2001:db8::1]:8443");
    CHECK (u.hostHeader == "[2001:db8::1]:8443");

    u = parseUrl ("http://[::1]/p");
    REQUIRE (u.valid);
    CHECK (u.host == "::1");
    CHECK (u.port == 80);
    CHECK (u.origin == "http://[::1]");
    CHECK (u.hostHeader == "[::1]");

    u = parseUrl ("https://example.com");
    REQUIRE (u.valid);
    CHECK (u.port == 443);
    CHECK (u.path.empty());
    CHECK (u.hostHeader == "example.com");

    u = parseUrl ("http://example.com:8080/a/b");
    REQUIRE (u.valid);
    CHECK (u.host == "example.com" && u.port == 8080 && u.path == "/a/b");
    CHECK (u.hostHeader == "example.com:8080");

    u = parseUrl ("http://example.com?x=1");
    REQUIRE (u.valid);
    CHECK (u.path == "/?x=1");

    for (const char* bad : { "http://example.com:abc/", "http://example.com:0/", "http://example.com:70000/", "http://example.com:/", "http://[::1]:x/",
                             "http://[::1/", "http://[::1]x/", "http://::1/", "http:///path", "ftp://example.com/", "example.com", "http://user@example.com/" })
    {
        const auto r = parseUrl (bad);
        CHECK (! r.valid);
        CHECK (! r.error.empty());
    }
}

//==============================================================================
// E10-22: job persistence and lookup
ROVOX_TEST (core, jobs_persist_the_final_state_and_never_announce_queued_after_it)
{
    const auto dir = test::makeTempDir ("jobs");
    std::mutex m;
    std::map<std::string, std::vector<JobState>> seen;
    std::vector<std::string> ids;

    {
        JobManager jobs (dir, {}, [&] (const JobInfo& i)
        {
            std::lock_guard<std::mutex> lock (m);
            seen[i.id].push_back (i.state);
        });

        for (int n = 0; n < 300; ++n)
        {
            JobSpec spec;
            spec.kind = "analysis";
            spec.pool = "analysis";
            ids.push_back (jobs.submit (spec, [] (JobContext&) -> Result<json> { return json { { "ok", true } }; }).id);
        }

        for (auto& id : ids)
            REQUIRE (jobs.get (id, std::chrono::seconds (30)).has_value());

        jobs.shutdown();
    }

    for (auto& id : ids)
    {
        std::ifstream in (dir / (id + ".json"));
        const auto j = json::parse (in, nullptr, false);
        REQUIRE (! j.is_discarded());
        CHECK (j.value ("state", "") == "succeeded");

        bool finished = false;

        for (auto st : seen[id])
        {
            CHECK (! (finished && ! isFinished (st)));      // "queued" / "running" after the final state
            finished = finished || isFinished (st);
        }

        CHECK (finished);
    }

    for (auto& f : fs::directory_iterator (dir))
        CHECK (f.path().extension() == ".json");            // no .tmp left behind
}

ROVOX_TEST (core, job_lookup_refuses_ids_that_are_paths_and_a_bad_record_does_not_stop_the_start)
{
    const auto root = test::makeTempDir ("jobget");
    const auto dir = root / "jobs";
    fs::create_directories (dir);
    std::ofstream (root / "secret.json") << R"({"job_id":"secret","state":"succeeded","tenant_id":"other"})";
    std::ofstream (dir / "bad.json") << R"({"job_id":5,"state":["queued"]})";
    std::ofstream (dir / "bad2.json") << R"({"job_id":"job_x","state":"queued","progress":"fast"})";

    JobManager jobs (dir);
    CHECK (! jobs.get ("../secret").has_value());
    CHECK (! jobs.get ("..").has_value());
    CHECK (! jobs.get ("job_../../secret").has_value());
    CHECK (! jobs.get ("").has_value());
    CHECK (jobs.list().empty());                            // the unreadable records were skipped

    JobSpec spec;
    spec.kind = "analysis";
    spec.pool = "analysis";
    const auto id = jobs.submit (spec, [] (JobContext&) -> Result<json> { return json { { "ok", true } }; }).id;
    REQUIRE (jobs.get (id, std::chrono::seconds (10)).has_value());
}

ROVOX_TEST (core, request_guard_allowed_hosts_loopback_ranges_and_same_host_rule)
{
    using namespace guard;

    // the whole 127.0.0.0/8 and ::1 are loopback, whatever the case
    CHECK (isLoopbackName ("LOCALHOST"));
    CHECK (isLoopbackName ("127.0.0.2"));
    CHECK (isLoopbackName ("[::1]"));
    CHECK (isLoopbackBind ("127.1.2.3"));
    CHECK (! isLoopbackName ("127.0.0.1.evil.example"));
    CHECK (! isLoopbackName ("127.example"));
    CHECK (! isLoopbackBind ("0.0.0.0"));

    // a reverse proxy forwarding Host: daw.example.com to a loopback bind
    CHECK (! hostAllowed (true, "daw.example.com"));
    CHECK (hostAllowed (true, "daw.example.com", { "daw.example.com" }));
    CHECK (hostAllowed (true, "DAW.example.com:8443", { "daw.example.com" }));                  // no port in the entry = any port
    CHECK (hostAllowed (true, "daw.example.com:8443", { "daw.example.com:8443" }));
    CHECK (! hostAllowed (true, "daw.example.com:9", { "daw.example.com:8443" }));
    CHECK (hostAllowed (true, "daw.example.com", {}, { "https://daw.example.com" }));           // the host of an allowed origin is an allowed Host
    CHECK (! hostAllowed (true, "evil.example", { "daw.example.com" }, { "https://daw.example.com" }));
    CHECK (hostAllowed (false, "anything.example"));

    // requestAllowed: the Origin of the proxied page passes once the Host does
    CHECK (requestAllowed (true, "https://daw.example.com", "daw.example.com", { "https://daw.example.com" }));
    CHECK (! requestAllowed (true, "https://daw.example.com", "daw.example.com", {}));
    CHECK (requestAllowed (true, "https://daw.example.com", "daw.example.com", {}, { "daw.example.com" }) == false);   // Origin not listed and loopback bind: not the page itself
    CHECK (requestAllowed (false, "http://server:4810", "server:4810", {}));                     // the same-host rule off loopback, as for REST/WS
    CHECK (! requestAllowed (false, "http://evil.example", "server:4810", {}));
}
