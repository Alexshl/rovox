// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E11-01: API keys (format, hash, expiry, revocation), the Authenticator (legacy token, open mode) and scopes as a set.
#include "Auth.h"
#include "KeyStore.h"
#include "Origin.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <sys/stat.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using namespace rovox;

namespace
{
std::string readAll (const fs::path& p)
{
    std::ifstream in (p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path keyFile (const std::string& tag)
{
    return test::makeTempDir (tag) / "auth" / "keys.json";
}

KeyStore::CreateSpec spec (const std::string& tenant, ScopeSet scopes, const std::string& label = "test")
{
    KeyStore::CreateSpec s;
    s.label = label;
    s.tenant = tenant;
    s.scopes = scopes;
    return s;
}
} // namespace

ROVOX_TEST (auth, key_format_roundtrips_and_rejects_malformed_keys)
{
    const auto id = newKeyId();
    const auto secret = newKeySecret();
    CHECK (id.size() == 12);
    CHECK (id.rfind ("k_", 0) == 0);
    CHECK (secret.size() == 43);
    CHECK (newKeySecret() != secret);

    const auto key = formatApiKey (id, secret);
    CHECK (key.rfind ("rvx_k_", 0) == 0);
    auto parsed = parseApiKey (key);
    REQUIRE (parsed.has_value());
    CHECK (parsed->id == id);
    CHECK (parsed->secret == secret);

    CHECK (! parseApiKey ("").has_value());
    CHECK (! parseApiKey ("rvx_").has_value());
    CHECK (! parseApiKey ("xxx_" + key.substr (4)).has_value());
    CHECK (! parseApiKey (key.substr (0, key.size() - 1)).has_value());
    CHECK (! parseApiKey (key + "a").has_value());
    CHECK (! parseApiKey (formatApiKey ("k_ABCDEFGHIJ", secret)).has_value());          // uppercase is not in the id alphabet
    CHECK (! parseApiKey (formatApiKey (id, secret.substr (0, 42) + "!")).has_value());

    const auto hash = hashKeySecret (secret);
    CHECK (hash.rfind ("sha256:", 0) == 0);
    CHECK (hash.size() == 7 + 64);
    CHECK (hashKeySecret (secret) == hash);
    CHECK (hashKeySecret (secret + "x") != hash);

    CHECK (constantTimeEquals ("abc", "abc"));
    CHECK (! constantTimeEquals ("abc", "abd"));
    CHECK (! constantTimeEquals ("abc", "ab"));
}

ROVOX_TEST (auth, scope_names_and_aliases)
{
    auto all = ScopeSet::parse ({ "catalog:read", "projects:read", "projects:write", "render", "delivery", "admin" });
    REQUIRE (all.ok());
    CHECK (all.value() == ScopeSet::all());
    CHECK (all.value().names().size() == 6);

    auto read = ScopeSet::parse ({ "read" });
    REQUIRE (read.ok());
    CHECK (read.value().has (Scope::projectsRead));
    CHECK (read.value().has (Scope::catalogRead));
    CHECK (! read.value().has (Scope::projectsWrite));

    auto write = ScopeSet::parse ({ "write" });
    REQUIRE (write.ok());
    CHECK (write.value().has (Scope::projectsWrite));
    CHECK (! write.value().has (Scope::render));
    CHECK (! write.value().has (Scope::admin));

    CHECK (! ScopeSet::parse ({ "root" }).ok());
    CHECK (Principal::local().tenant == "local");
    CHECK (Principal::local().scopes == ScopeSet::all());
}

ROVOX_TEST (auth, created_key_resolves_to_its_tenant_and_scopes_and_the_secret_is_never_stored)
{
    const auto file = keyFile ("auth-create");
    KeyStore store (file);
    auto created = store.create (spec ("acme", ScopeSet::of ({ Scope::catalogRead, Scope::projectsRead }), "flute"));
    REQUIRE (created.ok());
    const auto key = created.value().key;
    const auto secret = parseApiKey (key)->secret;
    CHECK (created.value().record["id"] == parseApiKey (key)->id);

    Authenticator auth (&store, "");
    CHECK (! auth.open());

    for (auto presented : { "Bearer " + key, key, "bearer  " + key + " " })
    {
        auto p = auth.resolve (presented);
        REQUIRE (p.ok());
        CHECK (p.value().tenant == "acme");
        CHECK (p.value().keyId == parseApiKey (key)->id);
        CHECK (p.value().authMode == "token");
        CHECK (p.value().has (Scope::catalogRead));
        CHECK (p.value().has (Scope::projectsRead));
        CHECK (! p.value().has (Scope::render));
        CHECK (! p.value().has (Scope::admin));
    }

    // wrong secret, wrong id, garbage, nothing
    const auto wrong = formatApiKey (parseApiKey (key)->id, newKeySecret());
    const auto unknown = formatApiKey (newKeyId(), secret);

    for (auto bad : { "Bearer " + wrong, unknown, std::string ("rvx_nonsense"), std::string ("Bearer "), std::string() })
    {
        auto p = auth.resolve (bad);
        REQUIRE (! p.ok());
        CHECK (p.error().code == errc::unauthorized);
        CHECK (p.error().toJson().dump().find (secret) == std::string::npos);
    }

    // the file holds the hash only, with mode 0600
    const auto text = readAll (file);
    CHECK (text.find (secret) == std::string::npos);
    CHECK (text.find (key) == std::string::npos);
    CHECK (text.find (hashKeySecret (secret)) != std::string::npos);
    struct stat st {};
    REQUIRE (::stat (file.c_str(), &st) == 0);
    CHECK ((st.st_mode & 0777) == 0600);

    // list never returns a hash or a secret
    const auto listed = store.list();
    REQUIRE (listed.size() == 1);
    CHECK (! listed[0].contains ("hash"));
    CHECK (listed.dump().find (secret) == std::string::npos);
    CHECK (listed.dump().find ("sha256") == std::string::npos);
    CHECK (listed[0]["label"] == "flute");
    CHECK (listed[0]["last_used_at"].get<std::int64_t>() > 0);      // resolve above touched it

    // a second store on the same file sees the key
    KeyStore again (file);
    CHECK (Authenticator (&again, "").resolve (key).ok());
}

ROVOX_TEST (auth, expired_keys_are_refused)
{
    std::int64_t clock = 1'000'000;
    KeyStore store (keyFile ("auth-expiry"), [&] { return clock; });
    auto s = spec ("acme", ScopeSet::of ({ Scope::projectsRead }));
    s.expiresAt = clock + 100;
    auto created = store.create (s);
    REQUIRE (created.ok());

    Authenticator auth (&store, "");
    CHECK (auth.resolve (created.value().key).ok());
    clock += 99;
    CHECK (auth.resolve (created.value().key).ok());
    clock += 1;
    auto p = auth.resolve (created.value().key);
    REQUIRE (! p.ok());
    CHECK (p.error().code == errc::unauthorized);
}

ROVOX_TEST (auth, revocation_through_the_store_is_immediate)
{
    KeyStore store (keyFile ("auth-revoke"));
    auto created = store.create (spec ("acme", ScopeSet::of ({ Scope::projectsRead })));
    REQUIRE (created.ok());
    Authenticator auth (&store, "");
    auto p = auth.resolve (created.value().key);
    REQUIRE (p.ok());
    CHECK (auth.stillValid (p.value()));

    REQUIRE (store.revoke (created.value().record["id"]).ok());
    CHECK (! auth.resolve (created.value().key).ok());
    CHECK (! auth.stillValid (p.value()));
    CHECK (store.revoke (created.value().record["id"]).ok());               // idempotent
    CHECK (! store.revoke ("k_aaaaaaaaaa").ok());
    // still listed, marked revoked
    CHECK (store.list()[0].contains ("revoked_at"));
}

ROVOX_TEST (auth, a_hand_edit_of_the_key_file_takes_effect_within_a_second)
{
    const auto file = keyFile ("auth-edit");
    KeyStore store (file);
    auto created = store.create (spec ("acme", ScopeSet::of ({ Scope::projectsRead })));
    REQUIRE (created.ok());
    Authenticator auth (&store, "");
    REQUIRE (auth.resolve (created.value().key).ok());

    // edit the file behind the store's back: mark the key revoked
    auto doc = json::parse (readAll (file));
    doc["keys"][0]["revoked_at"] = 1;
    { std::ofstream out (file, std::ios::trunc); out << doc.dump (2) << "\n"; }

    std::this_thread::sleep_for (std::chrono::milliseconds (1100));
    auto p = auth.resolve (created.value().key);
    CHECK (! p.ok());

    // and a new key added by another process is picked up the same way
    KeyStore other (file);
    auto second = other.create (spec ("acme", ScopeSet::of ({ Scope::projectsRead })));
    REQUIRE (second.ok());
    std::this_thread::sleep_for (std::chrono::milliseconds (1100));
    CHECK (auth.resolve (second.value().key).ok());
}

ROVOX_TEST (auth, legacy_token_is_local_with_every_scope)
{
    KeyStore store (keyFile ("auth-legacy"));
    Authenticator auth (&store, "s3cret-token");
    CHECK (! auth.open());

    auto p = auth.resolve ("Bearer s3cret-token");
    REQUIRE (p.ok());
    CHECK (p.value().tenant == "local");
    CHECK (p.value().keyId == "legacy");
    CHECK (p.value().authMode == "legacy");
    CHECK (p.value().scopes == ScopeSet::all());
    CHECK (auth.resolve ("s3cret-token").ok());

    CHECK (! auth.resolve ("Bearer s3cret-tokeN").ok());
    CHECK (! auth.resolve ("").ok());
    CHECK (auth.resolve ("").error().code == errc::unauthorized);

    // keys work next to the legacy token
    auto created = store.create (spec ("acme", ScopeSet::of ({ Scope::render })));
    REQUIRE (created.ok());
    auto k = auth.resolve ("Bearer " + created.value().key);
    REQUIRE (k.ok());
    CHECK (k.value().tenant == "acme");
    CHECK (auth.stillValid (p.value()));

    // no key store at all
    Authenticator bare (nullptr, "tok");
    CHECK (bare.resolve ("tok").ok());
    CHECK (! bare.resolve (created.value().key).ok());
}

ROVOX_TEST (auth, with_no_token_and_no_keys_everyone_is_the_local_operator)
{
    KeyStore store (keyFile ("auth-open"));
    Authenticator auth (&store, "");
    CHECK (auth.open());

    for (auto presented : { std::string(), std::string ("Bearer whatever") })
    {
        auto p = auth.resolve (presented);
        REQUIRE (p.ok());
        CHECK (p.value().tenant == "local");
        CHECK (p.value().authMode == "open");
        CHECK (p.value().scopes == ScopeSet::all());
    }

    Authenticator noStore (nullptr, "");
    CHECK (noStore.resolve ("").ok());

    // the first key closes the server
    REQUIRE (store.create (spec ("acme", ScopeSet::of ({ Scope::render }))).ok());
    CHECK (! auth.open());
    CHECK (! auth.resolve ("").ok());
}

ROVOX_TEST (auth, a_server_that_required_credentials_stays_closed_when_the_key_file_disappears)
{
    // latched by itself: keys were seen once
    const auto file = keyFile ("auth-latch");
    KeyStore store (file);
    Authenticator seen (&store, "");
    REQUIRE (store.create (spec ("acme", ScopeSet::of ({ Scope::render }))).ok());
    CHECK (! seen.open());
    fs::remove (file);
    std::this_thread::sleep_for (std::chrono::milliseconds (1100));
    CHECK (! seen.open());
    CHECK (! seen.resolve ("").ok());

    // set by serve for a non-loopback bind, before any key exists
    KeyStore none (keyFile ("auth-required"));
    Authenticator exposed (&none, "");
    CHECK (exposed.open());
    exposed.setRequireCredentials (true);
    CHECK (! exposed.open());
    CHECK (! exposed.resolve ("").ok());
}

ROVOX_TEST (auth, an_unreadable_key_file_does_not_open_the_server)
{
    const auto file = keyFile ("auth-broken");
    fs::create_directories (file.parent_path());
    { std::ofstream out (file); out << "{ this is not json"; }
    KeyStore store (file);
    Authenticator auth (&store, "");
    CHECK (! auth.open());
    CHECK (! auth.resolve ("").ok());
}

ROVOX_TEST (auth, admin_can_only_be_granted_to_the_local_tenant)
{
    const auto file = keyFile ("auth-admin");
    KeyStore store (file);

    auto bad = store.create (spec ("acme", ScopeSet::of ({ Scope::projectsRead, Scope::admin })));
    REQUIRE (! bad.ok());
    CHECK (bad.error().code == errc::invalidParams);
    CHECK (store.list().empty());

    auto good = store.create (spec ("local", ScopeSet::of ({ Scope::admin })));
    REQUIRE (good.ok());
    auto p = Authenticator (&store, "").resolve (good.value().key);
    REQUIRE (p.ok());
    CHECK (p.value().has (Scope::admin));

    // other invalid input
    CHECK (! store.create (spec ("Bad Tenant", ScopeSet::of ({ Scope::render }))).ok());
    CHECK (! store.create (spec ("../x", ScopeSet::of ({ Scope::render }))).ok());
    CHECK (! store.create (spec ("acme", ScopeSet())).ok());

    // a hand-edited file cannot smuggle admin into another tenant either
    auto acme = store.create (spec ("acme", ScopeSet::of ({ Scope::render })));
    REQUIRE (acme.ok());
    auto doc = json::parse (readAll (file));

    for (auto& k : doc["keys"])
        if (k["tenant"] == "acme")
            k["scopes"].push_back ("admin");

    { std::ofstream out (file, std::ios::trunc); out << doc.dump (2) << "\n"; }
    KeyStore reloaded (file);
    auto q = Authenticator (&reloaded, "").resolve (acme.value().key);
    REQUIRE (q.ok());
    CHECK (q.value().has (Scope::render));
    CHECK (! q.value().has (Scope::admin));
}

ROVOX_TEST (auth, rotate_keeps_the_grant_and_retires_the_old_key)
{
    std::int64_t clock = 2'000'000;
    KeyStore store (keyFile ("auth-rotate"), [&] { return clock; });
    auto s = spec ("acme", ScopeSet::of ({ Scope::catalogRead, Scope::render }), "robot");
    s.quotaProfile = "small";
    auto first = store.create (s);
    REQUIRE (first.ok());
    Authenticator auth (&store, "");

    // with a grace period both keys work, then the old one expires
    auto second = store.rotate (first.value().record["id"], 60);
    REQUIRE (second.ok());
    CHECK (second.value().key != first.value().key);
    auto p = auth.resolve (second.value().key);
    REQUIRE (p.ok());
    CHECK (p.value().tenant == "acme");
    CHECK (p.value().scopes == ScopeSet::of ({ Scope::catalogRead, Scope::render }));
    CHECK (p.value().quotaProfile == "small");
    CHECK (auth.resolve (first.value().key).ok());
    clock += 60;
    CHECK (! auth.resolve (first.value().key).ok());
    CHECK (auth.resolve (second.value().key).ok());

    // no grace: the old key is revoked at once
    auto third = store.rotate (second.value().record["id"]);
    REQUIRE (third.ok());
    CHECK (! auth.resolve (second.value().key).ok());
    CHECK (auth.resolve (third.value().key).ok());

    // an already retired key cannot be rotated
    CHECK (! store.rotate (first.value().record["id"]).ok());
    CHECK (store.list().size() == 3);
}

ROVOX_TEST (auth, origin_carries_subject_and_client_only_when_set)
{
    Origin o { Origin::Kind::mcp, "mcp_1", "Claude" };
    auto plain = o.toJson();
    CHECK (! plain.contains ("subject"));
    CHECK (! plain.contains ("client"));

    o.subject = "user-42";
    o.client = "flute-teacher";
    auto full = o.toJson();
    CHECK (full["subject"] == "user-42");
    CHECK (full["client"] == "flute-teacher");
}

//==============================================================================
// E11-02: the signed session cookie and AuthRequest

namespace
{
std::string cookieOf (const std::string& signedValue)
{
    return "theme=dark; rovox_session=" + signedValue + "; other=1";
}
} // namespace

ROVOX_TEST (auth, session_cookie_signs_verifies_and_never_holds_the_token)
{
    const auto dir = test::makeTempDir ("auth-cookie");
    KeyStore store (dir / "auth" / "keys.json");
    Authenticator auth (&store, "s3cret-token");
    auth.setSessionKeyFile (dir / "auth" / "session.key");

    auto legacy = auth.resolve ("Bearer s3cret-token");
    REQUIRE (legacy.ok());
    const auto value = auth.issueSession (legacy.value());
    CHECK (value.rfind ("legacy.", 0) == 0);
    CHECK (value.find ("s3cret-token") == std::string::npos);

    AuthRequest req;
    req.cookie = cookieOf (value);
    auto viaCookie = auth.resolve (req);
    REQUIRE (viaCookie.ok());
    CHECK (viaCookie.value().keyId == "legacy");
    CHECK (viaCookie.value().authMode == "legacy");

    // the secret is on disk (32 bytes, 0600) and survives a new Authenticator
    const auto keyPath = dir / "auth" / "session.key";
    REQUIRE (fs::exists (keyPath));
    CHECK (fs::file_size (keyPath) == 32);
    struct stat st {};
    REQUIRE (::stat (keyPath.c_str(), &st) == 0);
    CHECK ((st.st_mode & 0777) == 0600);
    Authenticator again (&store, "s3cret-token");
    again.setSessionKeyFile (keyPath);
    CHECK (again.resolve (req).ok());

    // tampering: another key id, another expiry, another signature
    const auto dot1 = value.find ('.'), dot2 = value.find ('.', dot1 + 1);
    const auto exp = value.substr (dot1 + 1, dot2 - dot1 - 1);
    AuthRequest forged;
    forged.cookie = cookieOf ("k_aaaaaaaaaa" + value.substr (dot1));
    CHECK (! auth.resolve (forged).ok());
    forged.cookie = cookieOf ("legacy." + std::to_string (std::stoll (exp) + 1000) + value.substr (dot2));
    CHECK (! auth.resolve (forged).ok());
    forged.cookie = cookieOf (value.substr (0, value.size() - 1) + (value.back() == '0' ? "1" : "0"));
    CHECK (! auth.resolve (forged).ok());
    forged.cookie = cookieOf ("legacy");
    CHECK (! auth.resolve (forged).ok());

    // a cookie signed with another secret is refused
    Authenticator stranger (&store, "s3cret-token");
    stranger.setSessionKeyFile (test::makeTempDir ("auth-cookie-other") / "session.key");
    forged.cookie = cookieOf (stranger.issueSession (legacy.value()));
    CHECK (! auth.resolve (forged).ok());

    // open mode: nothing to log in to
    Authenticator open (nullptr, "");
    CHECK (open.issueSession (Principal::local()).empty());
}

ROVOX_TEST (auth, legacy_session_cookie_is_bound_to_the_token)
{
    const auto dir = test::makeTempDir ("auth-cookie-bound");
    KeyStore store (dir / "auth" / "keys.json");
    Authenticator before (&store, "token-one");
    before.setSessionKeyFile (dir / "auth" / "session.key");
    auto legacy = before.resolve ("Bearer token-one");
    REQUIRE (legacy.ok());

    AuthRequest req;
    req.cookie = cookieOf (before.issueSession (legacy.value()));
    CHECK (before.resolve (req).ok());

    // the same secret on disk, another server token: the old cookie no longer opens anything
    Authenticator after (&store, "token-two");
    after.setSessionKeyFile (dir / "auth" / "session.key");
    CHECK (! after.resolve (req).ok());
}

ROVOX_TEST (auth, session_cookie_expires_and_follows_the_key)
{
    const auto dir = test::makeTempDir ("auth-cookie-key");
    std::int64_t clock = 2'000'000;
    KeyStore store (dir / "auth" / "keys.json", [&] { return clock; });
    auto created = store.create (spec ("acme", ScopeSet::of ({ Scope::catalogRead })));
    REQUIRE (created.ok());
    Authenticator auth (&store, "");
    auth.setSessionKeyFile (dir / "auth" / "session.key");

    auto principal = auth.resolve (created.value().key);
    REQUIRE (principal.ok());
    const auto value = auth.issueSession (principal.value(), 100);
    CHECK (value.rfind (created.value().record["id"].get<std::string>() + ".", 0) == 0);
    AuthRequest req;
    req.cookie = "rovox_session=" + value;

    auto viaCookie = auth.resolve (req);
    REQUIRE (viaCookie.ok());
    CHECK (viaCookie.value().tenant == "acme");
    CHECK (viaCookie.value().authMode == "token");
    CHECK (viaCookie.value().scopes == ScopeSet::of ({ Scope::catalogRead }));

    clock += 99;
    CHECK (auth.resolve (req).ok());
    clock += 1;                                                 // exp reached
    auto expired = auth.resolve (req);
    REQUIRE (! expired.ok());
    CHECK (expired.error().code == errc::unauthorized);

    clock = 2'000'000;
    const auto fresh = auth.issueSession (principal.value());
    req.cookie = "rovox_session=" + fresh;
    CHECK (auth.resolve (req).ok());
    REQUIRE (store.revoke (created.value().record["id"]).ok());      // revoking the key ends its sessions
    CHECK (! auth.resolve (req).ok());
}

ROVOX_TEST (auth, auth_request_tries_header_cookie_and_query_and_old_cookie_is_legacy_only)
{
    const auto dir = test::makeTempDir ("auth-request");
    KeyStore store (dir / "auth" / "keys.json");
    auto created = store.create (spec ("acme", ScopeSet::of ({ Scope::render })));
    REQUIRE (created.ok());
    Authenticator auth (&store, "tok-legacy");
    auth.setSessionKeyFile (dir / "auth" / "session.key");

    AuthRequest byHeader;
    byHeader.authorization = "Bearer tok-legacy";
    CHECK (auth.resolve (byHeader).ok());

    AuthRequest byKey;
    byKey.authorization = "Bearer " + created.value().key;
    auto k = auth.resolve (byKey);
    REQUIRE (k.ok());
    CHECK (k.value().tenant == "acme");

    AuthRequest byQuery;
    byQuery.queryToken = "tok-legacy";
    CHECK (auth.resolve (byQuery).ok());
    byQuery.queryToken = "wrong";
    CHECK (! auth.resolve (byQuery).ok());

    AuthRequest oldCookie;
    oldCookie.cookie = "rovox_token=tok-legacy";
    CHECK (auth.resolve (oldCookie).ok());
    oldCookie.cookie = "rovox_token=" + created.value().key;      // a key never rides the old cookie
    CHECK (! auth.resolve (oldCookie).ok());

    // a wrong header does not block a valid cookie, and nothing at all is unauthorized
    AuthRequest mixed;
    mixed.authorization = "Bearer nope";
    mixed.cookie = "rovox_session=" + auth.issueSession (k.value());
    CHECK (auth.resolve (mixed).ok());
    CHECK (auth.resolve (AuthRequest {}).error().code == errc::unauthorized);

    CHECK (auth.challengeHeader().empty());
    CHECK (! auth.resourceMetadata().has_value());

    // open mode ignores credentials
    Authenticator open (nullptr, "");
    auto o = open.resolve (AuthRequest {});
    REQUIRE (o.ok());
    CHECK (o.value().authMode == "open");
}
