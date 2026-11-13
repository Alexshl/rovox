// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E8-06: job output files, signed download URLs, X-Rovox-Event-Id and two-secret webhook signatures.
#include "Hmac.h"
#include "Ids.h"
#include "JobCallback.h"
#include "JobFiles.h"
#include "Sha256.h"
#include "MockServer.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <stdlib.h>
#include <sys/stat.h>

using namespace rovox;
using test::header;

namespace
{
fs::path sourceFile (const std::string& content)
{
    const auto f = test::makeTempDir ("jf_src") / "mix.wav";
    std::ofstream (f, std::ios::binary) << content;
    return f;
}

/** "/files/t/job/name?exp=1&sig=ab" -> the pieces. */
struct Link
{
    std::string tenant, job, name, exp, sig;
};

Link parseLink (const std::string& path)
{
    Link l;
    const auto q = path.find ('?');
    const auto parts = path.substr (0, q);
    size_t pos = std::string ("/files/").size();

    auto next = [&] (char stop)
    {
        const auto end = parts.find (stop, pos);
        auto piece = parts.substr (pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? parts.size() : end + 1;
        return piece;
    };

    l.tenant = next ('/');
    l.job = next ('/');
    l.name = next ('/');
    const auto query = path.substr (q + 1);
    const auto amp = query.find ('&');
    l.exp = query.substr (4, amp - 4);
    l.sig = query.substr (amp + 1 + 4);
    return l;
}

const std::int64_t t0 = 1'800'000'000;
} // namespace

ROVOX_TEST (jobfiles, publish_copies_into_the_job_dir_and_writes_the_manifest_last)
{
    const auto data = test::makeTempDir ("jf_data");
    JobFiles files (data);
    const auto job = newId ("job");
    const auto r = files.publish ("jf_a", job, "mix.wav", sourceFile ("hello"));
    REQUIRE (r.ok());
    CHECK (r.value()["bytes"].get<int>() == 5);
    CHECK (r.value()["sha256"].get<std::string>() == sha256Hex ("hello"));
    CHECK (fs::is_regular_file (data / "tenants" / "jf_a" / "jobs" / job / "out" / "mix.wav"));

    // idempotent per (job, name): the same name again replaces the entry, another name is added
    REQUIRE (files.publish ("jf_a", job, "mix.wav", sourceFile ("hello!")).ok());
    REQUIRE (files.publish ("jf_a", job, "stem.wav", sourceFile ("x")).ok());
    const auto m = files.manifest ("jf_a", job);
    REQUIRE (m.ok());
    CHECK (m.value()["files"].size() == 2);
    CHECK (files.manifest ("jf_b", job).error().code == errc::notFound);

    // no partial leftovers next to the files
    size_t entries = 0;

    for (auto& e : fs::directory_iterator (data / "tenants" / "jf_a" / "jobs" / job / "out"))
        entries += e.path().filename().string()[0] == '.' ? 100 : 1;

    CHECK (entries == 2);
}

ROVOX_TEST (jobfiles, publish_refuses_bad_names_and_ids)
{
    JobFiles files (test::makeTempDir ("jf_bad"));
    const auto job = newId ("job");
    const auto src = sourceFile ("x");

    for (const char* name : { "../x", "a/b", ".hidden", "", "a b", "x\\y" })
        CHECK (files.publish ("jf_a", job, name, src).error().code == errc::invalidParams);

    CHECK (files.publish ("../jf_a", job, "a.wav", src).error().code == errc::invalidParams);
    CHECK (files.publish ("jf_a", "job_nope", "a.wav", src).error().code == errc::invalidParams);
    CHECK (files.publish ("jf_a", job, "a.wav", fs::path ("/nonexistent/file.wav")).error().code == errc::notFound);
    CHECK (files.signedPath ("jf_a", job, "../a", t0).empty());
}

ROVOX_TEST (jobfiles, signed_url_is_multi_use_until_it_expires)
{
    JobFiles files (test::makeTempDir ("jf_exp"));
    const auto job = newId ("job");
    REQUIRE (files.publish ("jf_a", job, "mix.wav", sourceFile ("data")).ok());
    const auto link = parseLink (files.signedPath ("jf_a", job, "mix.wav", t0 + 600));
    CHECK (link.tenant == "jf_a");
    CHECK (link.job == job);
    CHECK (link.name == "mix.wav");

    for (int use = 0; use < 3; ++use)                                       // multi-use
    {
        const auto r = files.resolve (link.tenant, link.job, link.name, link.exp, link.sig, t0 + use);
        REQUIRE (r.ok());
        CHECK (r.value().filename() == "mix.wav");
    }

    CHECK (files.resolve (link.tenant, link.job, link.name, link.exp, link.sig, t0 + 599).ok());
    CHECK (files.resolve (link.tenant, link.job, link.name, link.exp, link.sig, t0 + 600).error().code == errc::unauthorized);
    CHECK (files.resolve (link.tenant, link.job, link.name, link.exp, link.sig, t0 + 9999).error().code == errc::unauthorized);
}

ROVOX_TEST (jobfiles, tampered_or_foreign_links_are_refused)
{
    JobFiles files (test::makeTempDir ("jf_tamper"));
    const auto job = newId ("job"), other = newId ("job");
    REQUIRE (files.publish ("jf_a", job, "mix.wav", sourceFile ("a")).ok());
    REQUIRE (files.publish ("jf_b", job, "mix.wav", sourceFile ("b")).ok());
    REQUIRE (files.publish ("jf_a", job, "other.wav", sourceFile ("c")).ok());
    REQUIRE (files.publish ("jf_a", other, "mix.wav", sourceFile ("d")).ok());
    const auto l = parseLink (files.signedPath ("jf_a", job, "mix.wav", t0 + 600));
    const auto denied = [&] (const std::string& tenant, const std::string& jobId, const std::string& name, const std::string& exp, const std::string& sig)
    {
        const auto r = files.resolve (tenant, jobId, name, exp, sig, t0);
        return ! r.ok() && r.error().code == errc::unauthorized;
    };

    CHECK (files.resolve (l.tenant, l.job, l.name, l.exp, l.sig, t0).ok());
    CHECK (denied ("jf_b", l.job, l.name, l.exp, l.sig));                    // another tenant's copy of the same job id
    CHECK (denied (l.tenant, other, l.name, l.exp, l.sig));                  // another job
    CHECK (denied (l.tenant, l.job, "other.wav", l.exp, l.sig));             // another file
    CHECK (denied (l.tenant, l.job, l.name, std::to_string (t0 + 6000), l.sig));    // a longer expiry
    CHECK (denied (l.tenant, l.job, l.name, l.exp, ""));
    CHECK (denied (l.tenant, l.job, l.name, l.exp, std::string (64, '0')));
    CHECK (denied (l.tenant, l.job, l.name, l.exp, l.sig.substr (0, 63)));
    CHECK (denied (l.tenant, l.job, l.name, "abc", l.sig));
    CHECK (denied ("..", l.job, l.name, l.exp, l.sig));
    CHECK (denied (l.tenant, l.job, "..", l.exp, l.sig));
    CHECK (denied (l.tenant, l.job, ".partial-x", l.exp, l.sig));
}

ROVOX_TEST (jobfiles, key_is_kept_in_a_0600_file_and_survives_a_restart)
{
    const auto data = test::makeTempDir ("jf_key");
    const auto job = newId ("job");
    std::string path;

    {
        JobFiles files (data);
        REQUIRE (files.publish ("jf_a", job, "mix.wav", sourceFile ("a")).ok());
        path = files.signedPath ("jf_a", job, "mix.wav", t0 + 600);
    }

    const auto keyFile = data / "auth" / "files.key";
    REQUIRE (fs::is_regular_file (keyFile));
    CHECK (fs::file_size (keyFile) == 32);
    struct stat st {};
    REQUIRE (::stat (keyFile.c_str(), &st) == 0);
    CHECK ((st.st_mode & 0777) == 0600);

    JobFiles again (data);                                                  // a new process reads the same key: old links still work
    const auto l = parseLink (path);
    CHECK (again.resolve (l.tenant, l.job, l.name, l.exp, l.sig, t0).ok());

    JobFiles elsewhere (test::makeTempDir ("jf_key2"));                     // another server's key does not verify it
    CHECK (! elsewhere.resolve (l.tenant, l.job, l.name, l.exp, l.sig, t0).ok());
}

ROVOX_TEST (jobfiles, a_symlink_in_the_out_dir_is_never_served)
{
    const auto data = test::makeTempDir ("jf_link");
    JobFiles files (data);
    const auto job = newId ("job");
    REQUIRE (files.publish ("jf_a", job, "mix.wav", sourceFile ("a")).ok());
    const auto secret = test::makeTempDir ("jf_secret") / "private.txt";
    std::ofstream (secret) << "private";
    std::error_code ec;
    fs::create_symlink (secret, files.outDir ("jf_a", job) / "leak.txt", ec);
    REQUIRE (! ec);
    const auto l = parseLink (files.signedPath ("jf_a", job, "leak.txt", t0 + 600));
    const auto r = files.resolve (l.tenant, l.job, l.name, l.exp, l.sig, t0);
    CHECK (! r.ok());
    CHECK (r.error().code == errc::notFound);
}

ROVOX_TEST (jobfiles, a_valid_link_to_a_missing_file_is_not_found)
{
    JobFiles files (test::makeTempDir ("jf_miss"));
    const auto l = parseLink (files.signedPath ("jf_a", newId ("job"), "mix.wav", t0 + 600));
    const auto r = files.resolve (l.tenant, l.job, l.name, l.exp, l.sig, t0);
    CHECK (! r.ok());
    CHECK (r.error().code == errc::notFound);
}

//==============================================================================
ROVOX_TEST (jobfiles, webhook_carries_an_event_id_and_both_signatures_during_a_rotation)
{
    ::setenv ("ROVOX_TEST_JF_OLD", "old-secret-value", 1);
    ::setenv ("ROVOX_TEST_JF_NEW", "new-secret-value", 1);
    test::Mock mock;
    mock.script = { { 500, "{}" }, { 200, "{}" } };                          // one retry: the event id must stay the same
    std::mutex mutex;
    std::map<std::string, json> states;
    std::map<std::string, SecretRef> secrets { { "old", SecretRef { "ROVOX_TEST_JF_OLD", "" } }, { "new", SecretRef { "ROVOX_TEST_JF_NEW", "" } } };
    JobCallbackDispatcher d ([&] (const std::string& id, const json& st) { std::lock_guard<std::mutex> lock (mutex); states[id] = st; }, secrets);
    d.setDelayScale (0.001);

    CallbackTarget target;
    target.url = mock.url() + "/hook";
    target.secretName = "old";
    target.nextSecretName = "new";
    target.resolvedIp = "127.0.0.1";
    d.enqueueBody ("job_rot", "job.finished", "{\"a\":1}", target);

    for (int i = 0; i < 500 && mock.count() < 2; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    REQUIRE (mock.count() == 2);
    const auto first = mock.at (0), second = mock.at (1);
    const auto ts = header (first, "X-Rovox-Timestamp");
    const auto expected = JobCallbackDispatcher::signature ("old-secret-value", ts, first.body) + "," + JobCallbackDispatcher::signature ("new-secret-value", ts, first.body);
    CHECK (header (first, "X-Rovox-Signature") == expected);
    CHECK (isValidId ("x_" + header (first, "X-Rovox-Event-Id"), "x"));
    CHECK (header (first, "X-Rovox-Event-Id") == header (second, "X-Rovox-Event-Id"));
    CHECK (header (first, "X-Rovox-Signature").find ("old-secret-value") == std::string::npos);
}

ROVOX_TEST (jobfiles, one_secret_keeps_the_plain_signature_and_each_event_gets_its_own_id)
{
    ::setenv ("ROVOX_TEST_JF_ONE", "single-secret-value", 1);
    test::Mock mock;
    std::map<std::string, SecretRef> secrets { { "one", SecretRef { "ROVOX_TEST_JF_ONE", "" } } };
    JobCallbackDispatcher d ([] (const std::string&, const json&) {}, secrets);
    CallbackTarget target;
    target.url = mock.url() + "/hook";
    target.secretName = "one";
    target.resolvedIp = "127.0.0.1";
    d.enqueueBody ("job_one_a", "job.finished", "{}", target);
    d.enqueueBody ("job_one_b", "job.finished", "{}", target);

    for (int i = 0; i < 500 && mock.count() < 2; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    REQUIRE (mock.count() == 2);
    const auto a = mock.at (0), b = mock.at (1);
    CHECK (header (a, "X-Rovox-Signature") == JobCallbackDispatcher::signature ("single-secret-value", header (a, "X-Rovox-Timestamp"), a.body));
    CHECK (! header (a, "X-Rovox-Event-Id").empty());
    CHECK (header (a, "X-Rovox-Event-Id") != header (b, "X-Rovox-Event-Id"));
}

ROVOX_TEST (jobfiles, callback_secret_ref_next_must_name_a_secret_with_a_value)
{
    ::setenv ("ROVOX_TEST_JF_V1", "v1", 1);
    ::setenv ("ROVOX_TEST_JF_V2", "v2", 1);
    CallbackSettings settings;
    settings.allowHosts = { "hook.test" };
    settings.secrets = { { "a", SecretRef { "ROVOX_TEST_JF_V1", "" } }, { "b", SecretRef { "ROVOX_TEST_JF_V2", "" } } };
    const HostResolver resolver = [] (const std::string&, int) { return std::vector<std::string> { "93.184.216.34" }; };

    auto ok = validateCallback ({ { "url", "https://hook.test/x" }, { "secret_ref", "a" }, { "secret_ref_next", "b" } }, settings, resolver);
    REQUIRE (ok.ok());
    CHECK (ok.value().nextSecretName == "b");
    CHECK (! validateCallback ({ { "url", "https://hook.test/x" }, { "secret_ref", "a" }, { "secret_ref_next", "nope" } }, settings, resolver).ok());
    CHECK (! validateCallback ({ { "url", "https://hook.test/x" }, { "secret_ref", "a" }, { "secret_ref_next", 5 } }, settings, resolver).ok());
    CHECK (validateCallback ({ { "url", "https://hook.test/x" }, { "secret_ref", "a" } }, settings, resolver).ok());
}
