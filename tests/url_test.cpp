// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The SSRF guard of Url.* (address classes, resolution, redirects) and the upload.from_url tool. A local HTTP server on 127.0.0.1 plays the remote host;
// the fetch options (UrlGuard::allowLoopback, McpConfig::fetchAllowLoopback) are the test-only switch that lets the guard pass loopback, nothing a request can set.
// Tenant ids are unique to this file (`url_*`) because every test of the process shares one data directory.
#include "Gateway.h"
#include "JobManager.h"
#include "Mcp.h"
#include "MockServer.h"
#include "RenderJobs.h"
#include "TestFramework.h"
#include "TestSupport.h"
#include "UploadStore.h"
#include "Url.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include <httplib.h>

using namespace rovox;

namespace
{
const std::string audioBytes = std::string ("RIFF") + std::string (4000, 'x');

/** A remote host on 127.0.0.1 with a few canned routes. */
struct Site
{
    Site()
    {
        svr.Get ("/clip.wav", [] (const httplib::Request&, httplib::Response& res) { res.set_content (audioBytes, "audio/wav"); });
        svr.Get ("/noext", [] (const httplib::Request&, httplib::Response& res) { res.set_content (audioBytes, "audio/mpeg"); });
        svr.Get ("/page.wav", [] (const httplib::Request&, httplib::Response& res) { res.set_content ("<html></html>", "text/html"); });
        svr.Get ("/readme.txt", [] (const httplib::Request&, httplib::Response& res) { res.set_content ("hello", "text/plain"); });
        svr.Get ("/empty.wav", [] (const httplib::Request&, httplib::Response& res) { res.set_content ("", "audio/wav"); });
        svr.Get ("/chunked.wav", [] (const httplib::Request&, httplib::Response& res)
        {
            res.set_chunked_content_provider ("audio/wav", [] (size_t offset, httplib::DataSink& sink)
            {
                if (offset < 3000)
                    sink.write (std::string (1000, 'c').data(), 1000);
                else
                    sink.done();

                return true;
            });
        });
        svr.Get ("/relative", [] (const httplib::Request&, httplib::Response& res) { res.status = 302; res.set_header ("Location", "clip.wav"); });
        svr.Get ("/to-metadata", [] (const httplib::Request&, httplib::Response& res) { res.status = 302; res.set_header ("Location", "http://169.254.169.254/latest/meta-data/"); });
        svr.Get ("/to-localhost-name", [] (const httplib::Request&, httplib::Response& res) { res.status = 301; res.set_header ("Location", "http://localhost:1/x.wav"); });
        svr.Get ("/loop", [] (const httplib::Request&, httplib::Response& res) { res.status = 302; res.set_header ("Location", "/loop"); });
        svr.Get ("/ftp", [] (const httplib::Request&, httplib::Response& res) { res.status = 302; res.set_header ("Location", "ftp://example.com/a.wav"); });

        port = svr.bind_to_any_port ("127.0.0.1");
        thread = std::thread ([this] { svr.listen_after_bind(); });

        for (int i = 0; i < 200 && ! svr.is_running(); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    ~Site()
    {
        svr.stop();
        thread.join();
    }

    std::string url (const std::string& path) const { return "http://127.0.0.1:" + std::to_string (port) + path; }

    httplib::Server svr;
    std::thread thread;
    int port = 0;
};

FetchOptions loopbackOptions()
{
    FetchOptions o;
    o.guard.allowLoopback = true;
    o.connectTimeoutS = 3;
    o.readTimeoutS = 5;
    return o;
}

std::string readAll (const fs::path& p)
{
    std::ifstream in (p, std::ios::binary);
    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
}

/** Same shape as the isolation harness: tool calls block on the message thread, so they run on a client thread while this thread pumps. */
struct Harness
{
    explicit Harness (mcp::McpConfig cfg = {})
        : jobs (fs::path {}), service (env.pm, jobs, env.executor), server (env.pm, env.bus, env.hub, jobs, service, std::move (cfg))
    {
    }

    ~Harness() { jobs.shutdown(); }

    std::shared_ptr<mcp::Session> session (const std::string& tenant)
    {
        auto s = server.createSession (tenant);
        rpc (s, "initialize", { { "protocolVersion", "2025-06-18" }, { "capabilities", json::object() }, { "clientInfo", { { "name", "url" }, { "version", "1" } } } });
        mcp::RequestContext rc;
        server.handle (*s, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }, rc);
        return s;
    }

    json rpc (const std::shared_ptr<mcp::Session>& s, const std::string& method, json params)
    {
        json req { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", method }, { "params", std::move (params) } };
        std::atomic<bool> done { false };
        json out;

        std::thread t ([&]
        {
            mcp::RequestContext rc;
            auto r = server.handle (*s, req, rc);
            out = r ? *r : json (nullptr);
            done = true;
        });

        while (! done)
            test::pumpMessages (2);

        t.join();
        return out;
    }

    json tool (const std::shared_ptr<mcp::Session>& s, const std::string& name, json args = json::object())
    {
        auto r = rpc (s, "tools/call", { { "name", name }, { "arguments", std::move (args) } });

        if (! r.contains ("result"))
            return { { "ok", false }, { "protocol_error", r.value ("error", json (nullptr)) } };

        return r["result"]["structuredContent"];
    }

    test::Env env;
    JobManager jobs;
    JobService service;
    mcp::McpServer server;
};

struct Store
{
    Store() : dir (test::makeTempDir ("url_uploads")), store (dir / "uploads")
    {
        store.setTenantBase (dir);
    }

    fs::path dir;
    UploadStore store;
};

std::string errorCode (const json& r)
{
    return r.contains ("error") && r["error"].is_object() ? r["error"].value ("code", std::string()) : std::string();
}
} // namespace

//==============================================================================
ROVOX_TEST (url, address_classes)
{
    for (auto* ip : { "127.0.0.1", "127.9.9.9", "10.0.0.1", "172.16.5.5", "192.168.0.1", "169.254.169.254", "0.0.0.0", "100.64.0.1", "198.18.0.1", "224.0.0.1", "255.255.255.255",
                      "::1", "::", "fe80::1", "fd12::1", "ff02::1", "::ffff:10.0.0.1", "::ffff:169.254.169.254", "64:ff9b::a00:1", "2002:7f00:1::", "not-an-address" })
        CHECK (isPrivateAddress (ip));

    for (auto* ip : { "93.184.216.34", "8.8.8.8", "172.32.0.1", "2606:2800:220:1:248:1893:25c8:1946", "::ffff:8.8.8.8" })
        CHECK (! isPrivateAddress (ip));

    CHECK (isLoopbackAddress ("127.0.0.1"));
    CHECK (isLoopbackAddress ("::1"));
    CHECK (isLoopbackAddress ("::ffff:127.0.0.1"));
    CHECK (! isLoopbackAddress ("10.0.0.1"));
    CHECK (! isLoopbackAddress ("::2"));
    CHECK (! isLoopbackAddress ("169.254.169.254"));
}

ROVOX_TEST (url, host_check_resolves_then_judges)
{
    UrlGuard guard;
    int asked = 0;
    std::vector<std::string> answer;
    guard.resolver = [&] (const std::string&, int) { ++asked; return answer; };
    const auto url = parseUrl ("http://files.example.com/a.wav");
    REQUIRE (url.valid);

    answer = { "93.184.216.34" };
    CHECK (checkUrlHost (url, guard).ok());

    answer = { "93.184.216.34", "10.0.0.5" };       // one bad address among good ones is enough to refuse
    auto mixed = checkUrlHost (url, guard);
    CHECK (! mixed.ok());
    CHECK (mixed.error().code == errc::invalidParams);

    answer = { "169.254.169.254" };
    CHECK (! checkUrlHost (url, guard).ok());

    answer = { "127.0.0.1" };
    CHECK (! checkUrlHost (url, guard).ok());
    guard.allowLoopback = true;
    CHECK (checkUrlHost (url, guard).ok());

    answer = { "10.0.0.5" };                        // the loopback switch does not open the private ranges
    CHECK (! checkUrlHost (url, guard).ok());

    answer = {};
    CHECK (! checkUrlHost (url, guard).ok());
    CHECK (asked == 7);                             // one resolution per check above
}

ROVOX_TEST (url, fetch_refuses_loopback_unless_the_test_switch_is_on)
{
    Site site;
    const auto dest = test::makeTempDir ("url_fetch") / "a.bin";

    FetchOptions closed;
    auto r = fetchUrlToFile (site.url ("/clip.wav"), dest, closed);
    CHECK (! r.ok());
    CHECK (! fs::exists (dest));

    auto allowed = fetchUrlToFile (site.url ("/clip.wav"), dest, loopbackOptions());
    REQUIRE (allowed.ok());
    CHECK (allowed.value().bytes == audioBytes.size());
    CHECK (readAll (dest) == audioBytes);
    CHECK (allowed.value().contentType == "audio/wav");
}

ROVOX_TEST (url, fetch_only_http_and_https)
{
    const auto dest = test::makeTempDir ("url_fetch") / "a.bin";
    CHECK (! fetchUrlToFile ("file:///etc/passwd", dest, loopbackOptions()).ok());
    CHECK (! fetchUrlToFile ("ftp://example.com/a.wav", dest, loopbackOptions()).ok());
    CHECK (! fetchUrlToFile ("gopher://127.0.0.1/", dest, loopbackOptions()).ok());
    CHECK (! fetchUrlToFile ("http://user:pw@127.0.0.1/a.wav", dest, loopbackOptions()).ok());
    CHECK (! fs::exists (dest));
}

ROVOX_TEST (url, fetch_checks_every_redirect)
{
    Site site;
    const auto dest = test::makeTempDir ("url_fetch") / "a.bin";

    // by default no redirect is followed at all
    auto refused = fetchUrlToFile (site.url ("/relative"), dest, loopbackOptions());
    CHECK (! refused.ok());

    auto follow = loopbackOptions();
    follow.maxRedirects = 3;
    auto relative = fetchUrlToFile (site.url ("/relative"), dest, follow);
    REQUIRE (relative.ok());
    CHECK (relative.value().finalUrl == site.url ("/clip.wav"));
    CHECK (readAll (dest) == audioBytes);

    auto metadata = fetchUrlToFile (site.url ("/to-metadata"), dest, follow);       // a public-looking page that bounces to the cloud metadata address
    REQUIRE (! metadata.ok());
    CHECK (metadata.error().code == errc::invalidParams);
    CHECK (! fs::exists (dest));

    auto named = fetchUrlToFile (site.url ("/to-localhost-name"), dest, follow);     // localhost:1 is loopback (allowed here) but nothing listens: a failure, not a hang
    CHECK (! named.ok());

    auto ftp = fetchUrlToFile (site.url ("/ftp"), dest, follow);
    CHECK (! ftp.ok());

    auto o = loopbackOptions();
    o.maxRedirects = 2;
    auto loop = fetchUrlToFile (site.url ("/loop"), dest, o);
    REQUIRE (! loop.ok());
    CHECK (test::contains (loop.error().message, "redirect"));
}

ROVOX_TEST (url, fetch_limits_size_by_header_and_by_count)
{
    Site site;
    const auto dest = test::makeTempDir ("url_fetch") / "a.bin";
    auto o = loopbackOptions();
    o.maxBytes = 1000;

    auto known = fetchUrlToFile (site.url ("/clip.wav"), dest, o);        // Content-Length is known: refused before the body
    REQUIRE (! known.ok());
    CHECK (known.error().code == errc::fileTooLarge);
    CHECK (! fs::exists (dest));

    auto chunked = fetchUrlToFile (site.url ("/chunked.wav"), dest, o);   // no Content-Length: cut off while counting
    REQUIRE (! chunked.ok());
    CHECK (chunked.error().code == errc::fileTooLarge);
    CHECK (! fs::exists (dest));

    o.maxBytes = 10000;
    auto fine = fetchUrlToFile (site.url ("/chunked.wav"), dest, o);
    REQUIRE (fine.ok());
    CHECK (fine.value().bytes >= 3000);
}

ROVOX_TEST (url, fetch_accept_callback_and_cancel)
{
    Site site;
    const auto dest = test::makeTempDir ("url_fetch") / "a.bin";
    auto o = loopbackOptions();
    std::string seenType;
    o.accept = [&] (const std::string& type, std::int64_t length, const std::string&) -> Result<void>
    {
        seenType = type;
        return length >= 0 ? Result<void> (makeError (errc::invalidParams, "no")) : Result<void>();
    };

    auto refused = fetchUrlToFile (site.url ("/clip.wav"), dest, o);
    REQUIRE (! refused.ok());
    CHECK (refused.error().message == "no");
    CHECK (seenType == "audio/wav");

    auto c = loopbackOptions();
    c.cancelled = [] { return true; };
    auto cancelled = fetchUrlToFile (site.url ("/clip.wav"), dest, c);
    REQUIRE (! cancelled.ok());
    CHECK (cancelled.error().code == errc::cancelled);
    CHECK (! fs::exists (dest));

    auto missing = fetchUrlToFile (site.url ("/nothing-here"), dest, loopbackOptions());
    REQUIRE (! missing.ok());
    CHECK (test::contains (missing.error().message, "404"));
}

ROVOX_TEST (url, tool_downloads_into_the_tenants_uploads)
{
    Site site;
    Store uploads;
    mcp::McpConfig cfg;
    cfg.uploads = &uploads.store;
    cfg.fetchAllowLoopback = true;
    Harness h (cfg);
    auto a = h.session ("url_a");
    auto b = h.session ("url_b");

    auto r = h.tool (a, "upload.from_url", { { "url", site.url ("/clip.wav") }, { "wait_s", 20 } });
    REQUIRE (r.value ("ok", false));
    CHECK ((r["data"].value ("state", std::string()) == "succeeded"));
    const auto& result = r["data"]["result"];
    const std::string id = result.value ("upload_id", std::string());
    CHECK (! id.empty());
    CHECK ((result.value ("filename", std::string()) == "clip.wav"));
    CHECK ((result.value ("bytes", 0) == (int) audioBytes.size()));
    CHECK ((result.value ("detected", std::string()) == "audio"));
    CHECK (test::contains (result.value ("path", std::string()), "tenants/url_a/uploads/" + id));

    auto mine = uploads.store.info (id, "url_a");
    REQUIRE (mine.ok());
    CHECK (mine.value().complete);
    CHECK (! uploads.store.info (id, "url_b").ok());            // the other tenant sees nothing

    // a name from the content type when the URL has no usable extension
    auto typed = h.tool (a, "upload.from_url", { { "url", site.url ("/noext") }, { "wait_s", 20 } });
    REQUIRE (typed.value ("ok", false));
    CHECK ((typed["data"]["result"].value ("filename", std::string()) == "noext.mp3"));

    // a client-chosen name is sanitised
    auto named = h.tool (a, "upload.from_url", { { "url", site.url ("/clip.wav") }, { "filename", "../../etc/pass wd.wav" }, { "wait_s", 20 } });
    REQUIRE (named.value ("ok", false));
    CHECK ((named["data"]["result"].value ("filename", std::string()) == "pass_wd.wav"));

    // the same client_request_id is the same job
    auto first = h.tool (b, "upload.from_url", { { "url", site.url ("/clip.wav") }, { "client_request_id", "once" }, { "wait_s", 20 } });
    auto again = h.tool (b, "upload.from_url", { { "url", site.url ("/clip.wav") }, { "client_request_id", "once" }, { "wait_s", 20 } });
    REQUIRE (first.value ("ok", false));
    REQUIRE (again.value ("ok", false));
    CHECK ((first["data"].value ("id", std::string()) == again["data"].value ("id", std::string())));
    CHECK (! again["data"].value ("created", true));
}

ROVOX_TEST (url, tool_refuses_what_is_not_audio_a_project_or_an_archive)
{
    Site site;
    Store uploads;
    mcp::McpConfig cfg;
    cfg.uploads = &uploads.store;
    cfg.fetchAllowLoopback = true;
    Harness h (cfg);
    auto a = h.session ("url_a");

    auto html = h.tool (a, "upload.from_url", { { "url", site.url ("/page.wav") }, { "wait_s", 20 } });
    CHECK (! html.value ("ok", true));
    CHECK (errorCode (html) == errc::invalidParams);

    auto text = h.tool (a, "upload.from_url", { { "url", site.url ("/readme.txt") }, { "wait_s", 20 } });      // text/plain is allowed for .rpp only by its extension
    CHECK (! text.value ("ok", true));
    CHECK ((errorCode (text) == errc::invalidParams));

    auto empty = h.tool (a, "upload.from_url", { { "url", site.url ("/empty.wav") }, { "wait_s", 20 } });
    CHECK (! empty.value ("ok", true));

    auto big = h.tool (a, "upload.from_url", { { "url", site.url ("/clip.wav") }, { "max_bytes", 100 }, { "wait_s", 20 } });
    CHECK (! big.value ("ok", true));
    CHECK ((errorCode (big) == errc::fileTooLarge));

    auto badScheme = h.tool (a, "upload.from_url", { { "url", "file:///etc/passwd" } });
    CHECK (! badScheme.value ("ok", true));
    CHECK ((errorCode (badScheme) == errc::invalidParams));
}

ROVOX_TEST (url, tool_cannot_reach_loopback_in_production_configuration)
{
    Site site;
    Store uploads;
    mcp::McpConfig cfg;
    cfg.uploads = &uploads.store;            // fetchAllowLoopback stays false, as `serve` leaves it
    Harness h (cfg);
    auto a = h.session ("url_a");

    auto r = h.tool (a, "upload.from_url", { { "url", site.url ("/clip.wav") }, { "wait_s", 20 } });
    CHECK (! r.value ("ok", true));
    CHECK ((errorCode (r) == errc::invalidParams));

    auto metadata = h.tool (a, "upload.from_url", { { "url", "http://169.254.169.254/latest/meta-data/" }, { "wait_s", 20 } });
    CHECK (! metadata.value ("ok", true));
    CHECK ((errorCode (metadata) == errc::invalidParams));

    auto numeric = h.tool (a, "upload.from_url", { { "url", "http://2130706433/a.wav" }, { "wait_s", 20 } });      // 127.0.0.1 written as one number
    CHECK (! numeric.value ("ok", true));

    auto v6 = h.tool (a, "upload.from_url", { { "url", "http://[::1]/a.wav" }, { "wait_s", 20 } });
    CHECK (! v6.value ("ok", true));

    CHECK (site.svr.is_running());
}

ROVOX_TEST (url, tool_is_absent_without_an_upload_store)
{
    Harness h;
    auto a = h.session ("url_a");
    auto r = h.tool (a, "upload.from_url", { { "url", "http://example.com/a.wav" } });
    CHECK (! r.value ("ok", true));
}
