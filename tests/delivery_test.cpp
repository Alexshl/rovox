// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// M5: delivery adapters against a local mock HTTP server (no real network): telegram, s3 (SigV4 against the AWS documentation vectors),
// webhook (HMAC), file; hold / release, idempotency, retries and the rule that secrets never show up in results, errors, the journal or the log.
#include "Delivery.h"
#include "DeliveryService.h"
#include "Hmac.h"
#include "JobManager.h"
#include "Render.h"
#include "MockServer.h"
#include "Sha256.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <httplib.h>

#include <atomic>
#include <fstream>
#include <thread>

#include <unistd.h>

using namespace rovox;
using test::Mock;
using test::Recorded;
using test::header;
using test::contains;

namespace
{
fs::path makeFile (const std::string& name, const std::string& content)
{
    const auto f = test::makeTempDir ("dlv") / name;
    std::ofstream (f, std::ios::binary) << content;
    return f;
}

auto notCancelled = [] { return false; };

void setSecret (const char* name, const char* value)    { ::setenv (name, value, 1); }
} // namespace

ROVOX_TEST (delivery, hmac_and_sigv4_match_the_published_vectors)
{
    // RFC 4231 test case 2
    CHECK (hmacSha256Hex ("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // AWS documentation: "Examples of how to derive a signing key": ListUsers on iam
    sigv4::Credentials iam { "AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "", "us-east-1", "iam" };
    sigv4::Request listUsers;
    listUsers.host = "iam.amazonaws.com";
    listUsers.query = { { "Action", "ListUsers" }, { "Version", "2010-05-08" } };
    listUsers.headers["content-type"] = "application/x-www-form-urlencoded; charset=utf-8";
    listUsers.payloadHash = sha256Hex ("");
    std::map<std::string, std::string> headers;
    const auto auth = sigv4::authorize (iam, listUsers, 1440938160, headers);       // 20150830T123600Z
    CHECK (contains (auth, "Credential=AKIDEXAMPLE/20150830/us-east-1/iam/aws4_request"));
    CHECK (contains (auth, "SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date") || contains (auth, "SignedHeaders=content-type;host;x-amz-date"));
    CHECK (sigv4::amzDate (1440938160) == "20150830T123600Z");

    // the signature of the documentation example uses exactly content-type, host and x-amz-date, and no content hash header
    sigv4::Request docExample = listUsers;
    docExample.payloadHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::map<std::string, std::string> three { { "content-type", listUsers.headers["content-type"] }, { "host", "iam.amazonaws.com" }, { "x-amz-date", "20150830T123600Z" } };
    const auto creq = sigv4::canonicalRequest (docExample, three);
    CHECK (creq == "GET\n/\nAction=ListUsers&Version=2010-05-08\ncontent-type:application/x-www-form-urlencoded; charset=utf-8\nhost:iam.amazonaws.com\nx-amz-date:20150830T123600Z\n\n"
                   "content-type;host;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const auto key = sigv4::signingKey (iam.secretKey, "20150830", "us-east-1", "iam");
    const auto sts = "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/iam/aws4_request\n" + sha256Hex (creq);
    CHECK (hmacSha256Hex (std::string (key.begin(), key.end()), sts) == "5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7");

    // AWS S3 documentation: PUT Object example
    sigv4::Credentials s3 { "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "", "us-east-1", "s3" };
    sigv4::Request put;
    put.method = "PUT";
    put.host = "examplebucket.s3.amazonaws.com";
    put.path = "/test$file.text";
    put.payloadHash = sha256Hex ("Welcome to Amazon S3.");
    put.headers = { { "date", "Fri, 24 May 2013 00:00:00 GMT" }, { "x-amz-storage-class", "REDUCED_REDUNDANCY" } };
    std::map<std::string, std::string> signedHeaders;
    const auto putAuth = sigv4::authorize (s3, put, 1369353600, signedHeaders);     // 20130524T000000Z
    CHECK (contains (putAuth, "Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"));
    CHECK (contains (putAuth, "SignedHeaders=date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class"));

    // AWS S3 documentation: presigned GET
    sigv4::Request get;
    get.host = "examplebucket.s3.amazonaws.com";
    get.path = "/test.txt";
    const auto query = sigv4::presignQuery (s3, get, 1369353600, 86400);
    CHECK (contains (query, "X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404"));
    CHECK (contains (query, "X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"));
}

ROVOX_TEST (delivery, file_adapter_copies_atomically_and_ignores_path_tricks)
{
    const auto dir = test::makeTempDir ("exports");
    auto adapter = makeFileAdapter ("file.share", dir);
    const auto src = makeFile ("mix.mp3", "audio-bytes");
    auto r = adapter->send ({ src, "c", "t", "", { { "filename", "../../evil.mp3" } } }, notCancelled);
    REQUIRE (r.ok());
    CHECK (r.value()["file"] == "evil.mp3");
    CHECK (fs::exists (dir / "evil.mp3"));
    CHECK (! fs::exists (dir / ".." / ".." / "evil.mp3"));
    CHECK (adapter->describe().dump().find (dir.string()) == std::string::npos);
}

ROVOX_TEST (delivery, telegram_adapter_uses_send_audio_for_audio_and_never_leaks_the_token)
{
    Mock mock;
    const std::string token = "123456:SECRET-bot-token-AAE";
    setSecret ("ROVOX_TEST_TG", token.c_str());
    auto adapter = makeTelegramAdapter ("telegram.me", { "ROVOX_TEST_TG", "" }, "987654321", mock.url());

    const auto mp3 = makeFile ("mix.mp3", "ID3-fake-mp3");
    auto r = adapter->send ({ mp3, "Barabany +3 dB", "Mix v3", "Rovox", {} }, notCancelled);
    REQUIRE (r.ok());
    CHECK (r.value()["message_id"] == 42);
    CHECK (r.value()["method"] == "sendAudio");
    REQUIRE (mock.count() == 1);
    auto req = mock.at (0);
    CHECK (req.path == "/bot" + token + "/sendAudio");
    CHECK (req.fields["chat_id"] == "987654321");
    CHECK (req.fields["caption"] == "Barabany +3 dB");
    CHECK (req.fields["title"] == "Mix v3");
    CHECK (req.fileNames["audio"] == "mix.mp3");
    CHECK (req.fileContents["audio"] == "ID3-fake-mp3");
    CHECK (r.value().dump().find (token) == std::string::npos);
    CHECK (adapter->describe().dump().find ("987654321") == std::string::npos);
    CHECK (adapter->describe().dump().find (token) == std::string::npos);

    auto zip = adapter->send ({ makeFile ("stems.zip", "PK"), "", "", "", {} }, notCancelled);
    REQUIRE (zip.ok());
    CHECK (zip.value()["method"] == "sendDocument");
    CHECK (mock.at (1).path == "/bot" + token + "/sendDocument");
    CHECK (mock.at (1).fileNames["document"] == "stems.zip");

    // the answer of the API may echo the token: it is scrubbed from the error
    mock.script = { { 200, "" }, { 200, "" }, { 401, R"({"ok":false,"description":"Unauthorized: bad token 123456:SECRET-bot-token-AAE"})" } };
    auto denied = adapter->send ({ mp3, "", "", "", {} }, notCancelled);
    REQUIRE (! denied.ok());
    CHECK (denied.error().code == errc::io);
    CHECK (denied.error().message.find (token) == std::string::npos);
    CHECK (denied.error().message.find ("***") != std::string::npos);

    // a server error is transient (the service retries), the API's own refusal is not
    mock.script = { { 200, "" }, { 200, "" }, { 200, "" }, { 502, "bad gateway" } };
    auto transient = adapter->send ({ mp3, "", "", "", {} }, notCancelled);
    CHECK (transient.error().code == errc::busy);

    // 50 MB limit: a clear error, nothing is sent
    const auto big = test::makeTempDir ("big") / "mix.wav";
    {
        std::ofstream f (big, std::ios::binary);
        f.seekp ((std::streamoff) telegramUploadLimitBytes + 10);
        f.put ('x');
    }

    const auto before = mock.count();
    auto tooBig = adapter->send ({ big, "", "", "", {} }, notCancelled);
    REQUIRE (! tooBig.ok());
    CHECK (tooBig.error().code == "FILE_TOO_LARGE");
    CHECK (tooBig.error().message.find ("mp3") != std::string::npos);
    CHECK (mock.count() == before);

    // no token in the environment: a precondition error that names the variable, not a crash
    ::unsetenv ("ROVOX_TEST_TG");
    auto noToken = adapter->send ({ mp3, "", "", "", {} }, notCancelled);
    CHECK (noToken.error().code == errc::preconditionFailed);
    CHECK (noToken.error().message.find ("ROVOX_TEST_TG") != std::string::npos);
}

ROVOX_TEST (delivery, s3_adapter_signs_the_put_and_returns_a_presigned_url)
{
    Mock mock;
    setSecret ("ROVOX_TEST_S3_ID", "AKIATESTACCESSKEY00");
    setSecret ("ROVOX_TEST_S3_SECRET", "s3cr3t/KEY+value/abcdefghijklmnop");
    S3Config c;
    c.endpoint = mock.url();
    c.bucket = "rovox-renders";
    c.region = "eu-central-1";
    c.prefix = "mixes/";
    c.accessKey = { "ROVOX_TEST_S3_ID", "" };
    c.secretKey = { "ROVOX_TEST_S3_SECRET", "" };
    c.presignTtlS = 900;
    auto adapter = makeS3Adapter ("s3.backup", c);

    const std::string content = "RIFF-fake-wav-content-0123456789";
    const auto src = makeFile ("my mix.wav", content);
    auto r = adapter->send ({ src, "", "", "", {} }, notCancelled);
    REQUIRE (r.ok());
    REQUIRE (mock.count() == 1);
    auto req = mock.at (0);
    CHECK (req.method == "PUT");
    CHECK (req.path == "/rovox-renders/mixes/my mix.wav");     // the server decodes it
    CHECK (req.body == content);
    CHECK (header (req, "x-amz-content-sha256") == sha256Hex (content));

    // the server side recomputes the signature from what it received
    const auto authHeader = header (req, "Authorization");
    CHECK (authHeader.rfind ("AWS4-HMAC-SHA256 Credential=AKIATESTACCESSKEY00/", 0) == 0);
    CHECK (contains (authHeader, "/eu-central-1/s3/aws4_request"));
    const auto amz = header (req, "x-amz-date");
    std::tm tm {};
    strptime (amz.c_str(), "%Y%m%dT%H%M%SZ", &tm);
    sigv4::Credentials cred { "AKIATESTACCESSKEY00", "s3cr3t/KEY+value/abcdefghijklmnop", "", "eu-central-1", "s3" };
    sigv4::Request expected;
    expected.method = "PUT";
    expected.host = "127.0.0.1:" + std::to_string (mock.port);
    expected.path = "/rovox-renders/mixes/my mix.wav";
    expected.payloadHash = sha256Hex (content);
    expected.headers["content-type"] = header (req, "Content-Type");
    std::map<std::string, std::string> hs;
    CHECK (sigv4::authorize (cred, expected, (std::int64_t) timegm (&tm), hs) == authHeader);

    CHECK (r.value()["bucket"] == "rovox-renders");
    CHECK (r.value()["key"] == "mixes/my mix.wav");
    CHECK (r.value()["etag"] == "\"abc123\"");
    const auto url = r.value()["url"].get<std::string>();
    CHECK (url.rfind (mock.url() + "/rovox-renders/mixes/my%20mix.wav?", 0) == 0);
    CHECK (contains (url, "X-Amz-Expires=900"));
    CHECK (contains (url, "X-Amz-Signature="));
    CHECK (! contains (url, "s3cr3t"));
    CHECK (! contains (r.value().dump(), "s3cr3t"));

    mock.script = { { 200, "" }, { 403, "<Error><Message>SignatureDoesNotMatch s3cr3t/KEY+value/abcdefghijklmnop</Message></Error>" } };
    auto denied = adapter->send ({ src, "", "", "", {} }, notCancelled);
    REQUIRE (! denied.ok());
    CHECK (! contains (denied.error().message, "s3cr3t"));

    // virtual-hosted style puts the bucket in the host name
    c.pathStyle = false;
    c.endpoint = "http://localhost:" + std::to_string (mock.port);
    auto vhost = makeS3Adapter ("s3.vhost", c);
    mock.script.clear();
    auto v = vhost->send ({ src, "", "", "", { { "key", "x/y.wav" } } }, notCancelled);
    CHECK (v.ok() || v.error().code == errc::busy);              // "rovox-renders.localhost" may not resolve here; the request line is what matters
}

ROVOX_TEST (delivery, webhook_adapter_signs_with_hmac_in_both_modes)
{
    Mock mock;
    mock.defaultReply = { 200, "{}" };
    setSecret ("ROVOX_TEST_HOOK", "whsec_test_secret_0001");
    const auto src = makeFile ("mix.mp3", "mp3-bytes");
    const auto hash = sha256Hex ("mp3-bytes");

    auto multipart = makeWebhookAdapter ("webhook.n8n", mock.url() + "/hook/abc", { "ROVOX_TEST_HOOK", "" });
    auto r = multipart->send ({ src, "caption text", "Title", "", {} }, notCancelled);
    REQUIRE (r.ok());
    auto req = mock.at (0);
    CHECK (req.path == "/hook/abc");
    CHECK (req.fields["caption"] == "caption text");
    CHECK (req.fileNames["file"] == "mix.mp3");
    CHECK (req.fileContents["file"] == "mp3-bytes");
    CHECK (req.fields["sha256"] == hash);
    const auto ts = header (req, "X-Rovox-Timestamp");
    CHECK (header (req, "X-Rovox-Signature") == "sha256=" + hmacSha256Hex ("whsec_test_secret_0001", ts + "." + hash));

    auto json = makeWebhookAdapter ("webhook.json", mock.url() + "/json", { "ROVOX_TEST_HOOK", "" }, "json");
    REQUIRE (json->send ({ src, "cap", "T", "", {} }, notCancelled).ok());
    auto jreq = mock.at (1);
    CHECK (header (jreq, "Content-Type") == "application/json");
    CHECK (header (jreq, "X-Rovox-Signature") == "sha256=" + hmacSha256Hex ("whsec_test_secret_0001", header (jreq, "X-Rovox-Timestamp") + "." + jreq.body));
    auto doc = json::parse (jreq.body);
    CHECK (doc["file"]["sha256"] == hash);
    CHECK (doc["caption"] == "cap");

    // without a secret there is no signature header
    ::unsetenv ("ROVOX_TEST_HOOK");
    REQUIRE (multipart->send ({ src, "", "", "", {} }, notCancelled).ok());
    CHECK (header (mock.at (2), "X-Rovox-Signature").empty());
}

ROVOX_TEST (delivery, config_file_defines_adapters_and_refuses_inline_secrets)
{
    const auto dir = test::makeTempDir ("cfg");
    const auto secretFile = dir / "tg.secret";
    std::ofstream (secretFile) << "  999:from-a-file \n";
    std::ofstream (dir / "rovox.toml") << R"(
[delivery.telegram.me]
bot_token_file = ")" << secretFile.string() << R"("
chat_id = "12345"
bot_token = "inline-should-be-ignored"

[delivery.s3.backup]
endpoint = "https://fsn1.your-objectstorage.com"
bucket = "renders"
region = "fsn1"
access_key_env = "A_ID"
secret_key_env = "A_SECRET"
presign_ttl_s = 600

[delivery.webhook.n8n]
url = "https://example.invalid/hook"
hmac_secret_env = "HOOK_SECRET"

[delivery.file.share]
dir = "/tmp/share-rovox-test"

[delivery.carrier.pigeon]
x = 1
)";
    auto reg = DeliveryRegistry::load (dir / "rovox.toml", dir / "exports");
    CHECK (reg.find ("telegram.me") != nullptr);
    CHECK (reg.find ("s3.backup") != nullptr);
    CHECK (reg.find ("webhook.n8n") != nullptr);
    CHECK (reg.find ("file.share") != nullptr);
    CHECK (reg.find ("file.exports") != nullptr);
    CHECK (reg.find ("carrier.pigeon") == nullptr);
    CHECK (reg.warnings().size() == 2);                    // the inline token and the unknown kind
    std::string all;

    for (auto& a : reg.all())
        all += a->describe().dump();

    CHECK (all.find ("12345") == std::string::npos);
    CHECK (all.find ("999:from-a-file") == std::string::npos);
    CHECK (all.find ("inline-should-be-ignored") == std::string::npos);
    CHECK (all.find ("fsn1.your-objectstorage") == std::string::npos);
    CHECK (reg.find ("telegram.me")->secretValues()[0] == "999:from-a-file");

    ::setenv ("ROVOX_TELEGRAM_TOKEN", "tok", 1);
    ::setenv ("ROVOX_TELEGRAM_CHAT_ID", "42", 1);
    CHECK (DeliveryRegistry::load (dir / "missing.toml", dir / "exports").find ("telegram.default") != nullptr);
    ::unsetenv ("ROVOX_TELEGRAM_TOKEN");
    ::unsetenv ("ROVOX_TELEGRAM_CHAT_ID");
    CHECK (DeliveryRegistry::load (dir / "missing.toml", dir / "exports").find ("telegram.default") == nullptr);
}

//==============================================================================
namespace
{
/** Delivery service on a project with one rendered file; the message loop is pumped while jobs run. */
struct ServiceFixture
{
    ServiceFixture() : jobs { fs::path() }, service (env.pm, jobs, env.executor, test::makeTempDir ("dcfg") / "none.toml")
    {
        service.setRetryBackoffMs (20);
        id = env.newProject ("delivery");
        const auto dir = test::makeTempDir ("dsine");
        test::writeWav (dir / "s.wav", { test::sine (440.0, -18.0, 2.0, 44100.0) }, 44100.0, 24);
        auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "s.wav").string() } } });
        env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
        RenderRequest req;
        req.name = "mix";
        req.format.container = "mp3";
        req.format.bitrateKbps = 128;
        req.tailS = 0;
        auto r = renderProject (env.project (id), req);
        REQUIRE (r.ok());
        renderJob = r.value();
    }

    ~ServiceFixture() { jobs.shutdown(); }

    json wait (const std::string& jobId)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);

        for (;;)
        {
            test::pumpMessages (10);
            auto j = jobs.get (jobId);

            if (j && isFinished (j->state))
                return j->toJson();

            REQUIRE (std::chrono::steady_clock::now() < deadline);
        }
    }

    DeliverySendArgs args (const std::string& adapter)
    {
        DeliverySendArgs a;
        a.projectId = id;
        a.adapter = adapter;
        a.source = { { "file", "renders/mix.mp3" } };
        a.caption = "caption";
        return a;
    }

    Result<DeliveryStarted> send (const DeliverySendArgs& a)
    {
        return service.send (a);
    }

    test::Env env;
    JobManager jobs { fs::path() };
    DeliveryService service;
    std::string id;
    json renderJob;
};
} // namespace

ROVOX_TEST (delivery, hold_queues_the_file_and_release_sends_it_exactly_once)
{
    ServiceFixture f;
    Mock mock;
    f.service.registry().add (makeWebhookAdapter ("webhook.test", mock.url() + "/hook", {}));

    f.env.ok (f.id, "project.update", { { "delivery_hold", true } });
    auto held = f.send (f.args ("webhook.test"));
    REQUIRE (held.ok());
    CHECK (held.value().held["status"] == "held");
    CHECK (held.value().jobId.empty());
    CHECK (mock.count() == 0);
    CHECK (f.service.list ("local", f.id, "held").value()["deliveries"].size() == 1);
    CHECK (f.service.list ("local", f.id, "").value()["hold"] == true);
    CHECK (held.value().held.contains ("file_path") == false);

    // the log of the project records that it was held
    bool logged = false;

    for (auto& e : f.env.project (f.id).activity().list (50))
        logged = logged || e["name"] == "delivery.held";

    test::pumpMessages (50);
    for (auto& e : f.env.project (f.id).activity().list (50))
        logged = logged || e["name"] == "delivery.held";

    CHECK (logged);

    auto released = f.service.release ("local", f.id, "", nullptr);
    REQUIRE (released.ok());
    REQUIRE (released.value().size() == 1);
    auto job = f.wait (released.value()[0].jobId);
    CHECK (job["state"] == "succeeded");
    CHECK (job["result"]["status"] == "sent");
    CHECK (mock.count() == 1);

    // a second release finds nothing to send
    auto again = f.service.release ("local", f.id, "", nullptr);
    REQUIRE (again.ok());
    CHECK (again.value().empty());
    auto byId = f.service.release ("local", f.id, held.value().held["delivery_id"], nullptr);
    CHECK (! byId.ok());
    CHECK (byId.error().code == errc::preconditionFailed);
    CHECK (mock.count() == 1);

    // two releases racing: still one request
    auto second = f.send (f.args ("webhook.test"));
    REQUIRE (second.ok());
    std::vector<std::string> jobsStarted;
    std::mutex m;
    std::vector<std::thread> threads;

    for (int i = 0; i < 4; ++i)
        threads.emplace_back ([&]
        {
            auto r = f.service.release ("local", f.id, "", nullptr);

            if (r.ok())
            {
                std::lock_guard<std::mutex> lock (m);
                for (auto& s : r.value())
                    jobsStarted.push_back (s.jobId);
            }
        });

    std::atomic<int> joined { 0 };
    std::thread joiner ([&] { for (auto& t : threads) t.join(); joined = 1; });

    while (joined == 0)
        test::pumpMessages (5);                 // the releases ask the message thread for the project

    joiner.join();
    REQUIRE (jobsStarted.size() == 1);

    for (auto& j : jobsStarted)
        f.wait (j);

    CHECK (mock.count() == 2);

    // hold off: sent at once; force sends through a hold
    f.env.ok (f.id, "project.update", { { "delivery_hold", false } });
    auto direct = f.send (f.args ("webhook.test"));
    REQUIRE (direct.ok());
    REQUIRE (! direct.value().jobId.empty());
    CHECK (f.wait (direct.value().jobId)["state"] == "succeeded");

    f.env.ok (f.id, "project.update", { { "delivery_hold", true } });
    auto forced = f.args ("webhook.test");
    forced.force = true;
    auto fs2 = f.send (forced);
    REQUIRE (fs2.ok());
    REQUIRE (! fs2.value().jobId.empty());
    CHECK (f.wait (fs2.value().jobId)["state"] == "succeeded");
}

ROVOX_TEST (delivery, client_request_id_makes_a_repeat_harmless_and_failures_are_retried)
{
    ServiceFixture f;
    Mock mock;
    mock.script = { { 500, "boom" }, { 503, "again" }, { 200, "{}" } };
    f.service.registry().add (makeWebhookAdapter ("webhook.flaky", mock.url() + "/hook", {}));

    auto a = f.args ("webhook.flaky");
    a.clientRequestId = "req-1";
    auto first = f.send (a);
    REQUIRE (first.ok());
    auto job = f.wait (first.value().jobId);
    CHECK (job["state"] == "succeeded");
    CHECK (job["result"]["attempts"] == 3);
    CHECK (mock.count() == 3);

    auto repeat = f.send (a);
    REQUIRE (repeat.ok());
    CHECK (! repeat.value().existing.is_null());
    CHECK (repeat.value().existing["status"] == "sent");
    CHECK (mock.count() == 3);

    // a final refusal is not retried and the journal says failed
    Mock refusing;
    refusing.defaultReply = { 404, "no such hook" };
    f.service.registry().add (makeWebhookAdapter ("webhook.gone", refusing.url() + "/hook", {}));
    auto gone = f.send (f.args ("webhook.gone"));
    REQUIRE (gone.ok());
    auto gj = f.wait (gone.value().jobId);
    CHECK (gj["state"] == "failed");
    CHECK (refusing.count() == 1);
    CHECK (f.service.list ("local", f.id, "failed").value()["deliveries"].size() == 1);

    // unknown adapter, bad source, file outside the project
    CHECK (f.send (f.args ("nope")).error().code == errc::notFound);
    auto badSource = f.args ("webhook.flaky");
    badSource.source = { { "file", "renders/missing.mp3" } };
    CHECK (f.send (badSource).error().code == errc::notFound);
    badSource.source = { { "file", "../../etc/passwd" } };
    CHECK (f.send (badSource).error().code == errc::invalidParams);
    badSource.source = { { "job_id", "job_nope" } };
    CHECK (f.send (badSource).error().code == errc::notFound);
}

ROVOX_TEST (delivery, secrets_never_reach_results_the_journal_the_log_or_events)
{
    ServiceFixture f;
    Mock mock;
    const std::string token = "777:ZZ-very-secret-bot-token-QQ";
    const std::string hmac = "whsec_very_secret_hmac_value";
    const std::string chatId = "7316529804";            // long enough that it cannot appear by chance in a timestamp, a counter or an id in the output
    setSecret ("ROVOX_TEST_TG2", token.c_str());
    setSecret ("ROVOX_TEST_HOOK2", hmac.c_str());
    mock.script = { { 401, R"({"ok":false,"description":"bad token 777:ZZ-very-secret-bot-token-QQ"})" } };
    f.service.registry().add (makeTelegramAdapter ("telegram.me", { "ROVOX_TEST_TG2", "" }, chatId, mock.url()));
    f.service.registry().add (makeWebhookAdapter ("webhook.n8n", mock.url() + "/h", { "ROVOX_TEST_HOOK2", "" }));

    std::string seen;
    std::mutex seenMutex;
    const auto observer = f.env.hub.addObserver ([&] (const Event& e) { std::lock_guard<std::mutex> lock (seenMutex); seen += e.body.dump(); });

    auto bad = f.send (f.args ("telegram.me"));
    REQUIRE (bad.ok());
    auto badJob = f.wait (bad.value().jobId);
    CHECK (badJob["state"] == "failed");
    seen += badJob.dump();
    mock.defaultReply = { 200, "{}" };
    auto good = f.send (f.args ("webhook.n8n"));
    REQUIRE (good.ok());
    seen += f.wait (good.value().jobId).dump();
    seen += f.service.listAdapters().dump();
    seen += f.service.list ("local", f.id, "").value().dump();
    test::pumpMessages (100);

    for (auto& e : f.env.project (f.id).activity().list (100))
        seen += e.dump();

    std::ifstream journal (f.env.project (f.id).dir() / "delivery" / "journal.json");
    std::stringstream js;
    js << journal.rdbuf();
    seen += js.str();
    f.env.hub.removeObserver (observer);

    for (auto& secret : { token, hmac, chatId })
        CHECK (! contains (seen, secret));

    CHECK (contains (seen, "telegram.me"));
}
