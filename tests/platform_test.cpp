// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E9-01: hooks for an external platform: origin subject/client in history and events, lifecycle events (project.saved, media.added,
// project.closed) on the hub and through the event webhooks, and the one-time signed /open link.
#include "Auth.h"
#include "EventWebhooks.h"
#include "HttpServer.h"
#include "Ids.h"
#include "JobCallback.h"
#include "OpenLink.h"
#include "MockServer.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <thread>
#include <vector>

#include <stdlib.h>

using namespace rovox;
using test::header;

namespace
{
std::vector<Event> drain (Subscription& sub, int waitMs = 50)
{
    std::vector<Event> out;
    Event e;

    while (sub.pop (e, std::chrono::milliseconds (waitMs)))
        out.push_back (e);

    return out;
}

const Event* firstOfType (const std::vector<Event>& events, const std::string& type)
{
    for (const auto& e : events)
        if (e.body.value ("type", "") == type)
            return &e;

    return nullptr;
}

bool waitUntil (const std::function<bool()>& cond, int ms = 10000)
{
    for (int i = 0; i < ms / 10; ++i)
    {
        if (cond())
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    return cond();
}

/** A project with one mono sine imported through the command layer. Returns its id and the wav path. */
std::string projectWithSine (test::Env& env, const std::string& name)
{
    const auto id = env.newProject (name);
    const auto f = test::makeTempDir ("plat-sine") / "t1.wav";
    test::writeWav (f, { test::sine (440.0, -20.0, 1.0, 44100.0) }, 44100.0, 24);
    auto plan = env.ok (id, "import.plan", { { "paths", json::array ({ f.string() }) } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    return id;
}

std::int64_t nowS()
{
    return (std::int64_t) std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

//==============================================================================
ROVOX_TEST (platform, origin_subject_and_client_reach_history_list_and_the_event_stream)
{
    test::Env env;
    const auto id = env.newProject ("origin");
    auto sub = env.hub.subscribe ({ "local", id });

    Origin from { Origin::Kind::mcp, "mcp_77", "Platform agent", "user-42", "flute-teacher" };
    auto r = env.runAs (from, id, "track.create", { { "name", "Flute" } });
    REQUIRE (r.ok());

    // history.list: the undo step carries the origin of the caller, subject and client included
    const auto history = env.ok (id, "history.list");
    REQUIRE (! history["undo"].empty());
    const auto step = history["undo"].back();
    CHECK (step["origin"]["subject"] == "user-42");
    CHECK (step["origin"]["client"] == "flute-teacher");
    CHECK (step["origin"]["label"] == "Platform agent");

    // the event stream: project.changed has the same origin
    const auto events = drain (*sub);
    const auto* changed = firstOfType (events, "project.changed");
    REQUIRE (changed != nullptr);
    CHECK (changed->body["origin"]["subject"] == "user-42");
    CHECK (changed->body["origin"]["client"] == "flute-teacher");

    // an origin without them stays as before: the keys are absent, not empty
    auto plain = env.run (id, "track.create", { { "name", "Plain" } });
    REQUIRE (plain.ok());
    const auto after = env.ok (id, "history.list")["undo"].back();
    CHECK (! after["origin"].contains ("subject"));
    CHECK (! after["origin"].contains ("client"));
}

//==============================================================================
ROVOX_TEST (platform, save_publishes_project_saved_with_the_files_and_close_publishes_project_closed)
{
    test::Env env;
    auto sub = env.hub.subscribe ({ "local", "" });
    const auto id = projectWithSine (env, "lifecycle");
    drain (*sub, 10);                                       // creation, import and the media event of the setup

    env.ok (id, "project.save");
    auto events = drain (*sub);
    const auto* saved = firstOfType (events, "project.saved");
    REQUIRE (saved != nullptr);
    CHECK (saved->projectId == id);
    CHECK (saved->tenant == "local");
    CHECK (saved->body["project_id"] == id);
    CHECK (saved->body["revision"].get<std::uint64_t>() == env.project (id).revision());
    REQUIRE (saved->body["files"].is_array());

    bool hasManifest = false, hasMedia = false, hasAutosave = false;

    for (const auto& f : saved->body["files"])
    {
        const auto name = f.get<std::string>();
        CHECK (! name.empty() && name[0] != '/');            // relative to the project folder
        hasManifest = hasManifest || name == "rovox.json";
        hasMedia = hasMedia || name.rfind ("media/", 0) == 0;
        hasAutosave = hasAutosave || name.rfind ("autosave/", 0) == 0;
    }

    CHECK (hasManifest);
    CHECK (hasMedia);
    CHECK (! hasAutosave);

    drain (*sub, 10);
    REQUIRE (env.pm.close (id, SaveMode::discard).ok());
    events = drain (*sub);
    const auto* closed = firstOfType (events, "project.closed");
    REQUIRE (closed != nullptr);
    CHECK (closed->projectId == id);
    CHECK (closed->tenant == "local");
    CHECK (closed->body["project_id"] == id);
}

ROVOX_TEST (platform, import_apply_publishes_media_added_per_new_content_addressed_file_once)
{
    test::Env env;
    auto sub = env.hub.subscribe ({ "local", "" });
    const auto id = env.newProject ("media");
    drain (*sub, 10);

    const auto dir = test::makeTempDir ("plat-media");
    const auto f = dir / "a.wav";
    test::writeWav (f, { test::sine (330.0, -20.0, 1.0, 44100.0) }, 44100.0, 24);

    auto plan = env.ok (id, "import.plan", { { "paths", json::array ({ f.string() }) } });
    env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
    auto events = drain (*sub);
    const auto* added = firstOfType (events, "media.added");
    REQUIRE (added != nullptr);
    CHECK (added->tenant == "local");
    CHECK (added->body["project_id"] == id);
    CHECK (added->body["sha256"].get<std::string>().size() == 64);
    CHECK (added->body["path"].get<std::string>().rfind ("media/", 0) == 0);
    const auto sha = added->body["sha256"].get<std::string>();

    // the same content again is not new media
    auto again = env.ok (id, "import.plan", { { "paths", json::array ({ f.string() }) } });
    env.ok (id, "import.apply", { { "plan_id", again["plan_id"] } });
    events = drain (*sub);
    int repeats = 0;

    for (const auto& e : events)
        if (e.body.value ("type", "") == "media.added" && e.body.value ("sha256", "") == sha)
            ++repeats;

    CHECK (repeats == 0);
}

//==============================================================================
namespace
{
const char* hookSecret = "whsec_platform_events";

std::filesystem::path hookConfig (const std::filesystem::path& file, int port, const std::string& events)
{
    ::setenv ("ROVOX_TEST_PLATFORM_WH_SECRET", hookSecret, 1);
    std::ofstream out (file, std::ios::trunc);
    out << "[callbacks]\nallow_hosts = [\"hooks.test\"]\nallow_private = true\n"
        << "[callbacks.secrets.hook]\nenv = \"ROVOX_TEST_PLATFORM_WH_SECRET\"\n"
        << "[tenants.local.webhooks]\nurl = \"http://hooks.test:" << port << "/events\"\nsecret_ref = \"hook\"\nevents = [" << events << "]\n";
    return file;
}
} // namespace

ROVOX_TEST (platform, lifecycle_events_go_through_the_signed_event_webhook_and_stay_tenant_scoped)
{
    test::Mock mock;
    const auto dir = test::makeTempDir ("plat-hook");
    EventWebhooks::Options options;
    options.configFile = hookConfig (dir / "rovox.toml", mock.port, "\"project.*\", \"media.added\"");
    options.resolver = [] (const std::string&, int) { return std::vector<std::string> { "127.0.0.1" }; };

    test::Env env;
    EventWebhooks hooks (env.hub, options);
    const auto id = projectWithSine (env, "hooked");
    env.ok (id, "project.save");
    REQUIRE (env.pm.close (id, SaveMode::discard).ok());

    std::map<std::string, json> byType;

    REQUIRE (waitUntil ([&]
    {
        byType.clear();

        for (size_t i = 0; i < mock.count(); ++i)
        {
            const auto body = json::parse (mock.at (i).body);
            byType[body.value ("type", "")] = body;
        }

        return byType.count ("project.saved") == 1 && byType.count ("media.added") == 1 && byType.count ("project.closed") == 1;
    }));

    for (size_t i = 0; i < mock.count(); ++i)
    {
        const auto r = mock.at (i);
        const auto ts = header (r, "X-Rovox-Timestamp");
        CHECK (! ts.empty());
        CHECK (header (r, "X-Rovox-Signature") == JobCallbackDispatcher::signature (hookSecret, ts, r.body));     // the receiver can verify the HMAC
        CHECK (! test::contains (r.body, hookSecret));
    }

    const auto& saved = byType["project.saved"];
    CHECK (saved["tenant"] == "local");
    CHECK (saved["project_id"] == id);
    CHECK (saved["data"]["files"].is_array() && ! saved["data"]["files"].empty());
    CHECK (saved["data"]["revision"].is_number());
    CHECK (byType["media.added"]["data"]["sha256"].get<std::string>().size() == 64);
    CHECK (byType["project.closed"]["data"]["project_id"] == id);

    // another tenant's events never reach this tenant's webhook
    const auto before = mock.count();
    env.hub.publish ("other_tenant", "p_x", { { "type", "project.saved" }, { "project_id", "p_x" }, { "files", json::array() } });
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    CHECK (mock.count() == before);
}

//==============================================================================
namespace
{
struct OpenEnv
{
    OpenEnv() : dir (test::makeTempDir ("plat-open")), config (dir / "rovox.toml")
    {
        std::ofstream (config) << "[open_link]\nsecret = \"" << secret << "\"\n";
    }

    OpenGrant grant (const std::string& project = "p_one", std::int64_t lifetime = 120) const
    {
        OpenGrant g;
        g.tenant = "plat_t1";
        g.project = project;
        g.subject = "user-9";
        g.client = "web-agent";
        g.exp = nowS() + lifetime;
        g.jti = "n" + newUlid();
        return g;
    }

    std::string token (const OpenGrant& g) const { return OpenLinks::mint (secret, g); }

    const std::string secret = "open-link-shared-secret-for-tests";
    std::filesystem::path dir, config;
};
} // namespace

ROVOX_TEST (platform, open_link_redeems_once_binds_tenant_and_project_and_expires)
{
    OpenEnv e;
    OpenLinks links (e.dir, e.config);
    REQUIRE (links.enabled());

    const auto g = e.grant();
    const auto token = e.token (g);

    auto first = links.redeem (token, "p_one", nowS());
    REQUIRE (first.ok());
    CHECK (first.value().tenant == "plat_t1");
    CHECK (first.value().subject == "user-9");
    CHECK (first.value().client == "web-agent");
    CHECK (first.value().project == "p_one");
    CHECK (! first.value().scopes.has (Scope::admin));

    // single use
    auto second = links.redeem (token, "p_one", nowS());
    CHECK (! second.ok());
    CHECK (! test::contains (second.error().message, e.secret));

    // the used nonce survives a restart (a new instance reads the log)
    OpenLinks restarted (e.dir, e.config);
    CHECK (! restarted.redeem (token, "p_one", nowS()).ok());

    // another project than the one in the token
    const auto g2 = e.grant ("p_one");
    CHECK (! links.redeem (e.token (g2), "p_two", nowS()).ok());

    // after exp
    const auto g3 = e.grant ("p_one", 60);
    CHECK (! links.redeem (e.token (g3), "p_one", nowS() + 61).ok());

    // lifetime above 5 minutes is refused
    CHECK (! links.redeem (e.token (e.grant ("p_one", 301)), "p_one", nowS()).ok());
}

ROVOX_TEST (platform, open_link_rejects_forged_tampered_wrongly_keyed_and_operator_tokens)
{
    OpenEnv e;
    OpenLinks links (e.dir, e.config);

    const auto g = e.grant();
    const auto token = e.token (g);
    const auto dot = token.find ('.');
    REQUIRE (dot != std::string::npos);

    auto flip = [] (std::string s, size_t at) { s[at] = s[at] == 'a' ? 'b' : 'a'; return s; };
    CHECK (! links.redeem (flip (token, 4), "p_one", nowS()).ok());                    // payload changed, mac not
    CHECK (! links.redeem (flip (token, dot + 3), "p_one", nowS()).ok());              // mac changed
    CHECK (! links.redeem (token.substr (0, dot), "p_one", nowS()).ok());              // no mac
    CHECK (! links.redeem ("", "p_one", nowS()).ok());
    CHECK (! links.redeem (OpenLinks::mint ("another-secret", e.grant()), "p_one", nowS()).ok());

    // a link never carries the operator scope, nor the operator tenant
    auto admin = e.grant();
    admin.scopes = ScopeSet::all();
    CHECK (! links.redeem (e.token (admin), "p_one", nowS()).ok());
    auto bad = e.grant();
    bad.tenant = "Not A Tenant";
    CHECK (! links.redeem (e.token (bad), "p_one", nowS()).ok());

    // without a configured secret the feature is off
    OpenLinks off (e.dir, e.dir / "missing.toml");
    CHECK (! off.enabled());
    CHECK (! off.redeem (token, "p_one", nowS()).ok());
}

ROVOX_TEST (platform, open_session_cookie_resolves_to_the_tenant_subject_and_client_of_the_link)
{
    OpenEnv e;
    Authenticator auth (nullptr, "legacy-token");
    auth.setSessionKeyFile (e.dir / "auth" / "session.key");

    Principal p;
    p.tenant = "plat_t1";
    p.subject = "user-9";
    p.client = "web-agent";
    p.scopes = ScopeSet::of ({ Scope::projectsRead });
    const auto cookie = auth.issueOpenSession (p, 600);
    REQUIRE (! cookie.empty());

    AuthRequest req;
    req.cookie = "rovox_session=" + cookie;
    auto r = auth.resolve (req);
    REQUIRE (r.ok());
    CHECK (r.value().tenant == "plat_t1");
    CHECK (r.value().subject == "user-9");
    CHECK (r.value().client == "web-agent");
    CHECK (r.value().authMode == "open_link");
    CHECK (r.value().has (Scope::projectsRead));
    CHECK (! r.value().has (Scope::render));
    CHECK (! r.value().has (Scope::admin));

    // a tampered value (another tenant) does not verify
    auto forged = cookie;
    forged[6] = forged[6] == 'a' ? 'b' : 'a';
    AuthRequest bad;
    bad.cookie = "rovox_session=" + forged;
    CHECK (! auth.resolve (bad).ok());

    // an expired one does not either
    AuthRequest old;
    old.cookie = "rovox_session=" + auth.issueOpenSession (p, -5);
    CHECK (! auth.resolve (old).ok());
}

ROVOX_TEST (platform, http_open_sets_a_session_once_and_answers_403_for_reuse_wrong_project_and_garbage)
{
    OpenEnv e;
    test::Env env;
    Authenticator auth (nullptr, "legacy-token", e.config);
    ServerOptions o;
    o.port = 0;
    o.authenticator = &auth;
    o.sessionKeyFile = e.dir / "auth" / "session.key";
    HttpServer server (test::host(), env.pm, env.bus, env.hub, o);
    REQUIRE (server.start().ok());
    httplib::Client c ("127.0.0.1", server.port());

    const auto g = e.grant ("p_web");
    const auto token = e.token (g);
    auto first = c.Get ("/open?project=p_web&token=" + token);
    REQUIRE (first != nullptr);
    CHECK (first->status == 302);
    CHECK (first->get_header_value ("Location").rfind ("/?project=p_web", 0) == 0);
    CHECK (first->get_header_value ("Referrer-Policy") == "no-referrer");
    const auto setCookie = first->get_header_value ("Set-Cookie");
    REQUIRE (setCookie.rfind ("rovox_session=", 0) == 0);
    CHECK (setCookie.find ("HttpOnly") != std::string::npos);
    CHECK (setCookie.find (e.secret) == std::string::npos);

    // the cookie is a session of the link's tenant and subject
    const auto value = setCookie.substr (0, setCookie.find (';'));
    auto session = c.Get ("/api/session", httplib::Headers { { "Cookie", value } });
    REQUIRE (session != nullptr);
    const auto info = json::parse (session->body);
    CHECK (info["authenticated"] == true);
    CHECK (info["tenant"] == "plat_t1");
    CHECK (info["auth_mode"] == "open_link");

    // the second use of the same link: 403, no cookie
    auto second = c.Get ("/open?project=p_web&token=" + token);
    REQUIRE (second != nullptr);
    CHECK (second->status == 403);
    CHECK (second->get_header_value ("Set-Cookie").empty());

    // a valid link for another project, and garbage
    auto wrongProject = c.Get ("/open?project=p_other&token=" + e.token (e.grant ("p_web")));
    REQUIRE (wrongProject != nullptr);
    CHECK (wrongProject->status == 403);
    auto garbage = c.Get ("/open?project=p_web&token=nonsense");
    REQUIRE (garbage != nullptr);
    CHECK (garbage->status == 403);
    auto missing = c.Get ("/open");
    REQUIRE (missing != nullptr);
    CHECK (missing->status == 403);

    server.stop();
}
