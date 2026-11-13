// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E11-01: API keys (format, hash, expiry, revocation), the Authenticator (legacy token, open mode) and scopes as a set.
#include "Auth.h"
#include "ExternalAuth.h"
#include "Hmac.h"
#include "KeyStore.h"
#include "Origin.h"
#include "TestFramework.h"
#include "TestSupport.h"
#include "MockServer.h"       // last: httplib pulls zlib.h, whose macros break choc_zlib.h (Tracktion, via TestSupport.h) if it comes first

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>

#include <sys/stat.h>

#include <atomic>
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

//==============================================================================
// E11-08: external authentication (trusted proxy, introspection, JWT). No network: injected fetchers and fixed keys.
namespace
{
fs::path writeConfig (const std::string& tag, const std::string& text)
{
    const auto file = test::makeTempDir (tag) / "rovox.toml";
    std::ofstream out (file);
    out << text;
    return file;
}

const char* proxyToml = R"([auth]
mode = "trusted_proxy"
[auth.trusted_proxy]
allow = ["10.0.0.0/8", "fd00::/8"]
secret = "proxy-secret"
max_skew_s = 60
)";

AuthRequest proxyRequest (const std::string& addr, std::int64_t ts, const std::string& path = "/api/projects", const std::string& tenant = "acme",
                          const std::string& scopes = "read,render", const std::string& secret = "proxy-secret")
{
    const std::string method = "GET", subject = "user-7", client = "flute";
    AuthRequest r;
    r.remoteAddr = addr;
    r.method = method;
    r.path = path;
    r.headers["x-rovox-tenant"] = tenant;
    r.headers["x-rovox-subject"] = subject;
    r.headers["x-rovox-scopes"] = scopes;
    r.headers["x-rovox-client"] = client;
    r.headers["x-rovox-ts"] = std::to_string (ts);
    r.headers["x-rovox-signature"] = hmacSha256Hex (secret, std::to_string (ts) + "\n" + method + "\n" + path + "\n" + tenant + "\n" + subject + "\n" + scopes + "\n" + client);
    return r;
}

std::string b64url (const std::string& in)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;

    for (unsigned char c : in)
    {
        acc = (acc << 8) | c;
        bits += 8;

        while (bits >= 6)
        {
            bits -= 6;
            out += a[(acc >> bits) & 63];
        }
    }

    if (bits > 0)
        out += a[(acc << (6 - bits)) & 63];

    return out;
}

std::string bnBytes (EVP_PKEY* k, const char* name)
{
    BIGNUM* bn = nullptr;
    EVP_PKEY_get_bn_param (k, name, &bn);
    std::string out ((size_t) BN_num_bytes (bn), '\0');
    BN_bn2bin (bn, reinterpret_cast<unsigned char*> (out.data()));
    BN_free (bn);
    return out;
}

struct TestKey
{
    EVP_PKEY* key = nullptr;
    std::string kid, kty;

    TestKey (const std::string& id, bool ec) : kid (id), kty (ec ? "EC" : "RSA")
    {
        key = ec ? EVP_PKEY_Q_keygen (nullptr, nullptr, "EC", "P-256") : EVP_PKEY_Q_keygen (nullptr, nullptr, "RSA", (size_t) 2048);
    }

    ~TestKey() { EVP_PKEY_free (key); }
    TestKey (const TestKey&) = delete;
    TestKey& operator= (const TestKey&) = delete;

    json jwk() const
    {
        json j = json::object();
        j["kid"] = kid;
        j["kty"] = kty;

        if (kty == "RSA")
        {
            j["n"] = b64url (bnBytes (key, "n"));
            j["e"] = b64url (bnBytes (key, "e"));
            return j;
        }

        unsigned char pub[65];
        size_t len = 0;
        EVP_PKEY_get_octet_string_param (key, "pub", pub, sizeof (pub), &len);
        j["crv"] = "P-256";
        j["x"] = b64url (std::string (reinterpret_cast<char*> (pub + 1), 32));
        j["y"] = b64url (std::string (reinterpret_cast<char*> (pub + 33), 32));
        return j;
    }

    std::string sign (const std::string& message) const
    {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit (ctx, nullptr, EVP_sha256(), nullptr, key);
        size_t len = 0;
        EVP_DigestSign (ctx, nullptr, &len, reinterpret_cast<const unsigned char*> (message.data()), message.size());
        std::string sig (len, '\0');
        EVP_DigestSign (ctx, reinterpret_cast<unsigned char*> (sig.data()), &len, reinterpret_cast<const unsigned char*> (message.data()), message.size());
        sig.resize (len);
        EVP_MD_CTX_free (ctx);

        if (kty == "EC")
        {
            const auto* p = reinterpret_cast<const unsigned char*> (sig.data());
            ECDSA_SIG* es = d2i_ECDSA_SIG (nullptr, &p, (long) sig.size());
            const BIGNUM* r = nullptr;
            const BIGNUM* sv = nullptr;
            ECDSA_SIG_get0 (es, &r, &sv);
            std::string raw (64, '\0');
            BN_bn2binpad (r, reinterpret_cast<unsigned char*> (raw.data()), 32);
            BN_bn2binpad (sv, reinterpret_cast<unsigned char*> (raw.data()) + 32, 32);
            ECDSA_SIG_free (es);
            return raw;
        }

        return sig;
    }

    std::string jwt (const json& claims, const std::string& alg = "") const
    {
        json header = json::object();
        header["alg"] = alg.empty() ? std::string (kty == "EC" ? "ES256" : "RS256") : alg;
        header["typ"] = "JWT";
        header["kid"] = kid;
        const auto head = b64url (header.dump());
        const auto body = b64url (claims.dump());
        return head + "." + body + "." + b64url (sign (head + "." + body));
    }
};

/** A scripted identity provider: counts the calls, answers from the fields. */
struct FakeIdp
{
    std::atomic<int> introspections { 0 }, jwksFetches { 0 };
    std::mutex mutex;
    int introspectStatus = 200;
    std::string introspectBody, jwksBody, lastIntrospectBody, lastAuthorization;

    HttpFetcher fetcher()
    {
        return [this] (const std::string& method, const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) -> HttpReply
        {
            std::lock_guard<std::mutex> lock (mutex);

            if (method == "POST" && url == "https://idp.test/introspect")
            {
                ++introspections;
                lastIntrospectBody = body;
                lastAuthorization = headers.count ("Authorization") != 0 ? headers.at ("Authorization") : std::string();
                return { introspectStatus, introspectBody };
            }

            if (method == "GET" && url == "https://idp.test/jwks.json")
            {
                ++jwksFetches;
                return { jwksBody.empty() ? 0 : 200, jwksBody };
            }

            return {};
        };
    }
};

const char* introspectionToml = R"([auth]
mode = "introspection"
[auth.introspection]
url = "https://idp.test/introspect"
client_id = "rovox"
client_secret = "idp-secret"
jwks_url = "https://idp.test/jwks.json"
issuer = "https://idp.test"
audience = "rovox-api"
resource = "https://rovox.example.com/"
)";

AuthRequest bearer (const std::string& token)
{
    AuthRequest r;
    r.authorization = "Bearer " + token;
    return r;
}
} // namespace

ROVOX_TEST (auth, external_cidr_matching_v4_v6_and_boundaries)
{
    auto net = parseCidr ("10.0.0.0/8");
    REQUIRE (net.has_value());
    CHECK (cidrContains (*net, "10.0.0.0"));
    CHECK (cidrContains (*net, "10.255.255.255"));
    CHECK (! cidrContains (*net, "11.0.0.0"));
    CHECK (! cidrContains (*net, "9.255.255.255"));
    CHECK (! cidrContains (*net, "::1"));
    CHECK (! cidrContains (*net, "not an address"));
    CHECK (cidrContains (*net, "::ffff:10.1.2.3"));        // IPv4-mapped counts as IPv4

    auto odd = parseCidr ("192.168.1.128/25");
    REQUIRE (odd.has_value());
    CHECK (cidrContains (*odd, "192.168.1.200"));
    CHECK (! cidrContains (*odd, "192.168.1.127"));

    auto single = parseCidr ("203.0.113.5");
    REQUIRE (single.has_value());
    CHECK (cidrContains (*single, "203.0.113.5"));
    CHECK (! cidrContains (*single, "203.0.113.6"));

    auto v6 = parseCidr ("fd00::/8");
    REQUIRE (v6.has_value());
    CHECK (cidrContains (*v6, "fd12:3456::1"));
    CHECK (! cidrContains (*v6, "fe80::1"));
    CHECK (! cidrContains (*v6, "10.0.0.1"));

    auto all = parseCidr ("0.0.0.0/0");
    REQUIRE (all.has_value());
    CHECK (cidrContains (*all, "198.51.100.1"));

    CHECK (! parseCidr ("10.0.0.0/33").has_value());
    CHECK (! parseCidr ("10.0.0.0/").has_value());
    CHECK (! parseCidr ("nonsense/8").has_value());
    CHECK (! parseCidr ("").has_value());
}

ROVOX_TEST (auth, trusted_proxy_accepts_signed_requests_and_rejects_the_rest)
{
    const auto cfg = writeConfig ("auth-proxy", proxyToml);
    Authenticator auth (nullptr, "", cfg);
    std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });
    CHECK (! auth.open());

    auto ok = auth.resolve (proxyRequest ("10.1.2.3", now));
    REQUIRE (ok.ok());
    CHECK (ok.value().tenant == "acme");
    CHECK (ok.value().subject == "user-7");
    CHECK (ok.value().client == "flute");
    CHECK (ok.value().authMode == "trusted_proxy");
    CHECK (ok.value().has (Scope::projectsRead));
    CHECK (ok.value().has (Scope::render));
    CHECK (! ok.value().has (Scope::projectsWrite));
    CHECK (auth.resolve (proxyRequest ("fd00::5", now)).ok());

    // a bad signature, another path than the signed one, another tenant, other scopes
    CHECK (auth.resolve (proxyRequest ("10.1.2.3", now, "/api/projects", "acme", "read,render", "other-secret")).error().code == errc::unauthorized);
    auto movedPath = proxyRequest ("10.1.2.3", now);
    movedPath.path = "/api/admin";
    CHECK (! auth.resolve (movedPath).ok());
    auto movedTenant = proxyRequest ("10.1.2.3", now);
    movedTenant.headers["x-rovox-tenant"] = "rival";
    CHECK (! auth.resolve (movedTenant).ok());
    auto movedScopes = proxyRequest ("10.1.2.3", now);
    movedScopes.headers["x-rovox-scopes"] = "write";
    CHECK (! auth.resolve (movedScopes).ok());

    // the timestamp: 60 s on either side passes, 61 s does not
    CHECK (auth.resolve (proxyRequest ("10.1.2.3", now - 60)).ok());
    CHECK (auth.resolve (proxyRequest ("10.1.2.3", now + 60)).ok());
    CHECK (! auth.resolve (proxyRequest ("10.1.2.3", now - 61)).ok());
    CHECK (! auth.resolve (proxyRequest ("10.1.2.3", now + 61)).ok());

    // invalid tenant id and unknown scope, even when correctly signed
    CHECK (! auth.resolve (proxyRequest ("10.1.2.3", now, "/x", "Bad Tenant!")).ok());
    CHECK (! auth.resolve (proxyRequest ("10.1.2.3", now, "/x", "acme", "teleport")).ok());
}

ROVOX_TEST (auth, trusted_proxy_admin_is_for_the_local_tenant_only)
{
    const auto cfg = writeConfig ("auth-proxy-admin", proxyToml);
    Authenticator auth (nullptr, "", cfg);
    const std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });

    auto acme = auth.resolve (proxyRequest ("10.0.0.9", now, "/x", "acme", "admin,render"));
    REQUIRE (acme.ok());
    CHECK (! acme.value().has (Scope::admin));
    CHECK (acme.value().has (Scope::render));

    // the tenant `local` is the operator: never claimable from outside unless allow_local is set
    CHECK (auth.resolve (proxyRequest ("10.0.0.9", now, "/x", "local", "admin")).error().code == errc::unauthorized);

    const auto allowing = writeConfig ("auth-proxy-allow-local", std::string ("[auth]\nallow_local = true\n") + std::string (proxyToml).substr (7));
    Authenticator permissive (nullptr, "", allowing);
    permissive.external().setClock ([&] { return now; });
    auto local = permissive.resolve (proxyRequest ("10.0.0.9", now, "/x", "local", "admin"));
    REQUIRE (local.ok());
    CHECK (local.value().has (Scope::admin));
}

ROVOX_TEST (auth, trusted_proxy_signature_covers_the_query_and_proxy_principals_expire)
{
    const auto cfg = writeConfig ("auth-proxy-target", proxyToml);
    Authenticator auth (nullptr, "", cfg);
    std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });

    // signed over "/api/x?a=1": the same path with another query (or none) is another request
    auto signedReq = proxyRequest ("10.0.0.9", now, "/api/x?a=1");
    signedReq.path = "/api/x";
    signedReq.target = "/api/x?a=1";
    auto p = auth.resolve (signedReq);
    REQUIRE (p.ok());

    auto otherQuery = signedReq;
    otherQuery.target = "/api/x?a=2";
    CHECK (! auth.resolve (otherQuery).ok());
    auto noQuery = signedReq;
    noQuery.target = "/api/x";
    CHECK (! auth.resolve (noQuery).ok());

    // a proxy principal is valid for a short time only (a WS opened through the proxy is dropped after it)
    CHECK (p.value().expiresAt == now + 60);
    CHECK (auth.stillValid (p.value()));
    now += 59;
    CHECK (auth.stillValid (p.value()));
    now += 1;
    CHECK (! auth.stillValid (p.value()));
}

ROVOX_TEST (auth, a_tenant_claim_of_local_from_a_provider_is_rejected_unless_allowed)
{
    TestKey rsa ("rsa-1", false);
    FakeIdp idp;
    idp.jwksBody = json { { "keys", json::array ({ rsa.jwk() }) } }.dump();
    const std::int64_t now = 1'800'000'000;

    json claims = json::object();
    claims["iss"] = "https://idp.test";
    claims["aud"] = "rovox-api";
    claims["exp"] = now + 3600;
    claims["tenant"] = "local";
    claims["scope"] = "admin render";

    {
        const auto cfg = writeConfig ("auth-local-claim", introspectionToml);
        Authenticator auth (nullptr, "", cfg);
        auth.external().setClock ([&] { return now; });
        auth.external().setFetcher (idp.fetcher());
        CHECK (auth.resolve (bearer (rsa.jwt (claims))).error().code == errc::unauthorized);
        idp.introspectBody = R"({"active":true,"tenant":"local","scope":"admin"})";
        CHECK (! auth.resolve (bearer ("opaque-local")).ok());
    }

    {
        const auto cfg = writeConfig ("auth-local-claim-allowed", "[auth]\nallow_local = true\n" + std::string (introspectionToml).substr (7));
        Authenticator auth (nullptr, "", cfg);
        auth.external().setClock ([&] { return now; });
        auth.external().setFetcher (idp.fetcher());
        auto p = auth.resolve (bearer (rsa.jwt (claims)));
        REQUIRE (p.ok());
        CHECK (p.value().has (Scope::admin));
    }
}

ROVOX_TEST (auth, jwt_sessions_expire_for_long_lived_connections_and_exp_nbf_have_a_leeway)
{
    TestKey rsa ("rsa-1", false);
    FakeIdp idp;
    idp.jwksBody = json { { "keys", json::array ({ rsa.jwk() }) } }.dump();
    const auto cfg = writeConfig ("auth-jwt-expiry", introspectionToml);
    Authenticator auth (nullptr, "", cfg);
    std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });
    auth.external().setFetcher (idp.fetcher());

    json claims = json::object();
    claims["iss"] = "https://idp.test";
    claims["aud"] = "rovox-api";
    claims["tenant"] = "acme";

    // a token living 20 s: the principal is good for 20 s (+ the leeway), then a WS must go
    claims["exp"] = now + 20;
    auto shortLived = auth.resolve (bearer (rsa.jwt (claims)));
    REQUIRE (shortLived.ok());
    CHECK (shortLived.value().expiresAt == now + 60);           // min (exp + leeway, now + cache TTL)
    CHECK (auth.stillValid (shortLived.value()));
    now += 61;
    CHECK (! auth.stillValid (shortLived.value()));

    // a long token is capped by the cache TTL: the identity provider is asked again after a minute
    now = 1'800'000'000;
    claims["exp"] = now + 3600;
    auto longLived = auth.resolve (bearer (rsa.jwt (claims)));
    REQUIRE (longLived.ok());
    CHECK (longLived.value().expiresAt == now + 60);

    // leeway: exp up to 60 s in the past and nbf up to 60 s ahead are tolerated, not more
    json expired = claims;
    expired["exp"] = now - 59;
    CHECK (auth.resolve (bearer (rsa.jwt (expired))).ok());
    expired["exp"] = now - 60;
    CHECK (! auth.resolve (bearer (rsa.jwt (expired))).ok());
    json early = claims;
    early["nbf"] = now + 60;
    CHECK (auth.resolve (bearer (rsa.jwt (early))).ok());
    early["nbf"] = now + 61;
    CHECK (! auth.resolve (bearer (rsa.jwt (early))).ok());

    // a configurable leeway
    const auto strict = writeConfig ("auth-jwt-leeway", std::string (introspectionToml) + "leeway_s = 0\n");
    Authenticator strictAuth (nullptr, "", strict);
    strictAuth.external().setClock ([&] { return now; });
    strictAuth.external().setFetcher (idp.fetcher());
    expired["exp"] = now - 1;
    CHECK (! strictAuth.resolve (bearer (rsa.jwt (expired))).ok());
}

ROVOX_TEST (auth, an_unparseable_config_fails_closed_instead_of_falling_back_to_token_mode)
{
    const auto cfg = writeConfig ("auth-broken", "[auth\nmode = ");
    Authenticator auth (nullptr, "", cfg);
    CHECK (! auth.open());
    CHECK (auth.external().mode() != AuthMode::token);
    CHECK (! auth.resolve (bearer ("anything")).ok());
    CHECK (! auth.resolve (bearer ("rvx_k_AAAAAAAAAA_secret")).ok());
}

ROVOX_TEST (auth, trusted_proxy_ignores_headers_from_outside_the_allowlist)
{
    const auto cfg = writeConfig ("auth-proxy-spoof", proxyToml);
    Authenticator auth (nullptr, "legacy-tok", cfg);
    const std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });

    // a perfectly signed request, but from 203.0.113.5: the headers are not a credential
    auto spoofed = proxyRequest ("203.0.113.5", now);
    auto r = auth.resolve (spoofed);
    CHECK (! r.ok());
    CHECK (r.error().code == errc::unauthorized);

    // with a valid bearer the request is that bearer's principal, never the one the headers claim
    spoofed.authorization = "Bearer legacy-tok";
    auto viaBearer = auth.resolve (spoofed);
    REQUIRE (viaBearer.ok());
    CHECK (viaBearer.value().tenant == "local");
    CHECK (viaBearer.value().authMode == "legacy");
    CHECK (viaBearer.value().subject.empty());

    // from an allowlisted address without any X-Rovox-* headers the request is an ordinary one
    AuthRequest plain;
    plain.remoteAddr = "10.1.1.1";
    plain.authorization = "Bearer legacy-tok";
    CHECK (auth.resolve (plain).value().authMode == "legacy");
    plain.authorization.clear();
    CHECK (! auth.resolve (plain).ok());
}

ROVOX_TEST (auth, trusted_proxy_fails_closed_without_a_secret)
{
    const auto cfg = writeConfig ("auth-proxy-nosecret", "[auth]\nmode = \"trusted_proxy\"\n[auth.trusted_proxy]\nallow = [\"10.0.0.0/8\"]\nsecret_ref = \"ROVOX_TEST_UNSET_SECRET_VAR\"\n");
    Authenticator auth (nullptr, "", cfg);
    const std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });
    CHECK (! auth.resolve (proxyRequest ("10.0.0.1", now, "/x", "acme", "read", "")).ok());
}

ROVOX_TEST (auth, native_keys_are_valid_only_in_the_token_mode)
{
    KeyStore store (keyFile ("auth-ext-native"));
    auto created = store.create (spec ("acme", ScopeSet::of ({ Scope::render })));
    REQUIRE (created.ok());
    const auto key = created.value().key;

    CHECK (Authenticator (&store, "").resolve (key).ok());

    const auto proxyCfg = writeConfig ("auth-ext-native-proxy", proxyToml);
    Authenticator proxied (&store, "tok", proxyCfg);
    CHECK (proxied.resolve (key).error().code == errc::unauthorized);
    CHECK (! proxied.resolve (bearer (key)).ok());
    CHECK (proxied.resolve ("tok").ok());                       // the legacy operator token works in every mode

    // in introspection mode a native key is refused without asking the provider
    FakeIdp idp;
    idp.introspectBody = R"({"active":true,"tenant":"acme"})";
    const auto cfg = writeConfig ("auth-ext-native-introspection", introspectionToml);
    Authenticator introspected (&store, "", cfg);
    introspected.external().setFetcher (idp.fetcher());
    CHECK (introspected.resolve (key).error().code == errc::unauthorized);
    CHECK (idp.introspections.load() == 0);

    // a session cookie of a native key does not outlive the switch to an external mode
    Authenticator tokenMode (&store, "");
    auto principal = tokenMode.resolve (key);
    REQUIRE (principal.ok());
    AuthRequest cookieOnly;
    cookieOnly.cookie = "rovox_session=" + tokenMode.issueSession (principal.value());
    CHECK (tokenMode.resolve (cookieOnly).ok());
    CHECK (! proxied.resolve (cookieOnly).ok());
}

ROVOX_TEST (auth, introspection_cache_honours_positive_and_negative_ttl)
{
    FakeIdp idp;
    const auto cfg = writeConfig ("auth-introspection-cache", introspectionToml);
    Authenticator auth (nullptr, "", cfg);
    std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });
    auth.external().setFetcher (idp.fetcher());

    idp.introspectBody = R"({"active":true,"sub":"u1","client_id":"app","tenant":"acme","scope":"read render openid"})";
    auto p = auth.resolve (bearer ("opaque-token-1"));
    REQUIRE (p.ok());
    CHECK (p.value().tenant == "acme");
    CHECK (p.value().subject == "u1");
    CHECK (p.value().client == "app");
    CHECK (p.value().authMode == "introspection");
    CHECK (p.value().has (Scope::render));
    CHECK (p.value().has (Scope::catalogRead));
    CHECK (idp.introspections.load() == 1);
    CHECK (test::contains (idp.lastIntrospectBody, "token=opaque-token-1"));
    CHECK (idp.lastAuthorization.rfind ("Basic ", 0) == 0);

    // within the TTL the provider is not asked again; after 61 s it is
    now += 59;
    CHECK (auth.resolve (bearer ("opaque-token-1")).ok());
    CHECK (idp.introspections.load() == 1);
    now += 2;
    CHECK (auth.resolve (bearer ("opaque-token-1")).ok());
    CHECK (idp.introspections.load() == 2);

    // an inactive token is remembered for about 5 s
    idp.introspectBody = R"({"active":false})";
    CHECK (! auth.resolve (bearer ("opaque-token-2")).ok());
    CHECK (! auth.resolve (bearer ("opaque-token-2")).ok());
    CHECK (idp.introspections.load() == 3);
    now += 4;
    CHECK (! auth.resolve (bearer ("opaque-token-2")).ok());
    CHECK (idp.introspections.load() == 3);
    now += 2;
    idp.introspectBody = R"({"active":true,"tenant":"acme"})";
    CHECK (auth.resolve (bearer ("opaque-token-2")).ok());     // the negative entry has expired
    CHECK (idp.introspections.load() == 4);
}

ROVOX_TEST (auth, introspection_positive_entry_never_outlives_exp_and_errors_are_not_cached)
{
    FakeIdp idp;
    const auto cfg = writeConfig ("auth-introspection-exp", introspectionToml);
    Authenticator auth (nullptr, "", cfg);
    std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });
    auth.external().setFetcher (idp.fetcher());

    idp.introspectBody = R"({"active":true,"tenant":"acme","exp":1800000010})";
    CHECK (auth.resolve (bearer ("short-lived")).ok());
    now += 9;
    CHECK (auth.resolve (bearer ("short-lived")).ok());
    CHECK (idp.introspections.load() == 1);
    now += 62;                                                  // past exp and the 60 s leeway: asked again, and the answer is "expired"
    CHECK (! auth.resolve (bearer ("short-lived")).ok());
    CHECK (idp.introspections.load() == 2);

    // a provider failure fails closed and is not cached: the next request asks again
    idp.introspectStatus = 503;
    CHECK (! auth.resolve (bearer ("flaky")).ok());
    CHECK (! auth.resolve (bearer ("flaky")).ok());
    CHECK (idp.introspections.load() == 4);
    idp.introspectStatus = 200;
    idp.introspectBody = R"({"active":true,"tenant":"acme"})";
    CHECK (auth.resolve (bearer ("flaky")).ok());

    // no tenant claim, or a malformed one, is no principal
    idp.introspectBody = R"({"active":true,"sub":"u"})";
    CHECK (! auth.resolve (bearer ("no-tenant")).ok());
    idp.introspectBody = R"({"active":true,"tenant":"Not Valid"})";
    CHECK (! auth.resolve (bearer ("bad-tenant")).ok());
    idp.introspectBody = "not json";
    CHECK (! auth.resolve (bearer ("garbage")).ok());

    // admin from a provider is stripped for foreign tenants
    idp.introspectBody = R"({"active":true,"tenant":"acme","scope":"admin render"})";
    auto p = auth.resolve (bearer ("greedy"));
    REQUIRE (p.ok());
    CHECK (! p.value().has (Scope::admin));
}

ROVOX_TEST (auth, introspection_over_real_http_posts_a_form_with_client_credentials)
{
    test::Mock idp;
    idp.defaultReply = { 200, R"({"active":true,"tenant":"acme","sub":"u9"})" };
    const auto cfg = writeConfig ("auth-introspection-http", "[auth]\nmode = \"introspection\"\n[auth.introspection]\nurl = \"" + idp.url() + "/oauth/introspect\"\nclient_id = \"rovox\"\nclient_secret = \"idp-secret\"\n");
    Authenticator auth (nullptr, "", cfg);

    auto p = auth.resolve (bearer ("t0k3n"));
    REQUIRE (p.ok());
    CHECK (p.value().subject == "u9");
    REQUIRE (idp.count() == 1);
    const auto got = idp.at (0);
    CHECK (got.method == "POST");
    CHECK (got.path == "/oauth/introspect");
    CHECK (test::contains (got.body, "token=t0k3n"));
    CHECK (test::contains (test::header (got, "Authorization"), "Basic "));
    CHECK (! test::contains (test::header (got, "Authorization"), "idp-secret"));     // base64 only, never the plain secret

    // a dead endpoint is a 401, not a hang
    Authenticator dead (nullptr, "", writeConfig ("auth-introspection-dead", "[auth]\nmode = \"introspection\"\n[auth.introspection]\nurl = \"http://127.0.0.1:1/introspect\"\n"));
    CHECK (! dead.resolve (bearer ("t0k3n")).ok());
}

ROVOX_TEST (auth, jwt_rs256_and_es256_are_verified_against_the_jwks)
{
    TestKey rsa ("rsa-1", false), ec ("ec-1", true);
    FakeIdp idp;
    idp.jwksBody = json { { "keys", json::array ({ rsa.jwk(), ec.jwk() }) } }.dump();
    const auto cfg = writeConfig ("auth-jwt", introspectionToml);
    Authenticator auth (nullptr, "", cfg);
    const std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });
    auth.external().setFetcher (idp.fetcher());

    json good = json::object();
    good["iss"] = "https://idp.test";
    good["aud"] = "rovox-api";
    good["exp"] = now + 3600;
    good["nbf"] = now - 10;
    good["sub"] = "u1";
    good["tenant"] = "acme";
    good["scope"] = "read render";
    good["client_id"] = "flute";

    auto viaRsa = auth.resolve (bearer (rsa.jwt (good)));
    REQUIRE (viaRsa.ok());
    CHECK (viaRsa.value().tenant == "acme");
    CHECK (viaRsa.value().subject == "u1");
    CHECK (viaRsa.value().client == "flute");
    CHECK (viaRsa.value().has (Scope::render));

    auto viaEc = auth.resolve (bearer (ec.jwt (good)));
    REQUIRE (viaEc.ok());
    CHECK (viaEc.value().tenant == "acme");

    // the audience may be an array
    json arrayAud = good;
    arrayAud["aud"] = json::array ({ "other", "rovox-api" });
    CHECK (auth.resolve (bearer (rsa.jwt (arrayAud))).ok());

    CHECK (idp.jwksFetches.load() == 1);                        // the key set is cached
    CHECK (idp.introspections.load() == 0);                     // a JWT does not go to the introspection endpoint
}

ROVOX_TEST (auth, jwt_with_wrong_claims_signature_or_algorithm_is_unauthorized)
{
    TestKey rsa ("rsa-1", false), stranger ("rsa-1", false);
    FakeIdp idp;
    idp.jwksBody = json { { "keys", json::array ({ rsa.jwk() }) } }.dump();
    const auto cfg = writeConfig ("auth-jwt-bad", introspectionToml);
    Authenticator auth (nullptr, "", cfg);
    const std::int64_t now = 1'800'000'000;
    auth.external().setClock ([&] { return now; });
    auth.external().setFetcher (idp.fetcher());

    json good = json::object();
    good["iss"] = "https://idp.test";
    good["aud"] = "rovox-api";
    good["exp"] = now + 3600;
    good["tenant"] = "acme";
    REQUIRE (auth.resolve (bearer (rsa.jwt (good))).ok());

    auto with = [&] (const char* name, const json& value) { json c = good; c[name] = value; return c; };
    CHECK (! auth.resolve (bearer (rsa.jwt (with ("iss", "https://evil.test")))).ok());
    CHECK (! auth.resolve (bearer (rsa.jwt (with ("aud", "someone-else")))).ok());
    CHECK (! auth.resolve (bearer (rsa.jwt (with ("aud", json::array ({ "a", "b" }))))).ok());
    CHECK (! auth.resolve (bearer (rsa.jwt (with ("exp", now - 61)))).ok());
    CHECK (! auth.resolve (bearer (rsa.jwt (with ("exp", now - 60)))).ok());
    CHECK (! auth.resolve (bearer (rsa.jwt (with ("nbf", now + 100)))).ok());
    CHECK (! auth.resolve (bearer (rsa.jwt (with ("tenant", "Not Valid")))).ok());

    json noExp = good;
    noExp.erase ("exp");
    CHECK (! auth.resolve (bearer (rsa.jwt (noExp))).ok());

    // signed with another key under the same kid
    CHECK (! auth.resolve (bearer (stranger.jwt (good))).ok());

    // a tampered payload keeps the old signature
    const auto token = rsa.jwt (good);
    const auto firstDot = token.find ('.');
    const auto secondDot = token.find ('.', firstDot + 1);
    json forged = good;
    forged["tenant"] = "rival";
    CHECK (! auth.resolve (bearer (token.substr (0, firstDot + 1) + b64url (forged.dump()) + token.substr (secondDot))).ok());

    // alg none, with and without a signature; HS256 keyed with the public material
    const auto noneHead = b64url (R"({"alg":"none","typ":"JWT"})");
    const auto body = b64url (good.dump());
    CHECK (! auth.resolve (bearer (noneHead + "." + body + ".")).ok());
    CHECK (! auth.resolve (bearer (noneHead + "." + body + ".AAAA")).ok());
    const auto hsHead = b64url (R"({"alg":"HS256","typ":"JWT","kid":"rsa-1"})");
    const auto hsMac = hmacSha256 (rsa.jwk()["n"].get<std::string>(), hsHead + "." + body);
    CHECK (! auth.resolve (bearer (hsHead + "." + body + "." + b64url (std::string (hsMac.begin(), hsMac.end())))).ok());

    // an RS256 header over an EC key (algorithm confusion)
    TestKey ec ("rsa-1", true);
    idp.jwksBody = json { { "keys", json::array ({ ec.jwk() }) } }.dump();
    Authenticator second (nullptr, "", cfg);
    second.external().setClock ([&] { return now; });
    second.external().setFetcher (idp.fetcher());
    CHECK (! second.resolve (bearer (ec.jwt (good, "RS256"))).ok());
    CHECK (second.resolve (bearer (ec.jwt (good))).ok());
}

ROVOX_TEST (auth, jwt_needs_a_pinned_issuer_and_audience_and_an_answering_issuer)
{
    TestKey rsa ("rsa-1", false);
    FakeIdp idp;
    idp.jwksBody = json { { "keys", json::array ({ rsa.jwk() }) } }.dump();
    const std::int64_t now = 1'800'000'000;
    json claims = json::object();
    claims["iss"] = "https://idp.test";
    claims["aud"] = "rovox-api";
    claims["exp"] = now + 60;
    claims["tenant"] = "acme";

    // no audience configured: JWTs are not accepted at all
    Authenticator unpinned (nullptr, "", writeConfig ("auth-jwt-unpinned", "[auth]\nmode = \"introspection\"\n[auth.introspection]\njwks_url = \"https://idp.test/jwks.json\"\nissuer = \"https://idp.test\"\n"));
    unpinned.external().setClock ([&] { return now; });
    unpinned.external().setFetcher (idp.fetcher());
    CHECK (! unpinned.resolve (bearer (rsa.jwt (claims))).ok());

    // the JWKS endpoint is down: 401, and the next request tries again
    FakeIdp down;
    Authenticator auth (nullptr, "", writeConfig ("auth-jwt-down", introspectionToml));
    auth.external().setClock ([&] { return now; });
    auth.external().setFetcher (down.fetcher());
    CHECK (! auth.resolve (bearer (rsa.jwt (claims))).ok());
    CHECK (! auth.resolve (bearer (rsa.jwt (claims))).ok());
    CHECK (down.jwksFetches.load() == 2);
    down.jwksBody = idp.jwksBody;
    CHECK (auth.resolve (bearer (rsa.jwt (claims))).ok());
}

ROVOX_TEST (auth, protected_resource_metadata_and_challenge_follow_the_mode)
{
    Authenticator introspected (nullptr, "", writeConfig ("auth-metadata", introspectionToml));
    CHECK (introspected.challengeHeader() == "Bearer resource_metadata=\"https://rovox.example.com/.well-known/oauth-protected-resource\"");
    auto meta = introspected.resourceMetadata();
    REQUIRE (meta.has_value());
    CHECK ((*meta)["resource"] == "https://rovox.example.com/");
    CHECK ((*meta)["authorization_servers"][0] == "https://idp.test");
    CHECK ((*meta)["bearer_methods_supported"][0] == "header");

    // nothing is advertised in the other modes, nor without a resource
    Authenticator proxied (nullptr, "", writeConfig ("auth-metadata-proxy", proxyToml));
    CHECK (proxied.challengeHeader().empty());
    CHECK (! proxied.resourceMetadata().has_value());
    Authenticator bare (nullptr, "", writeConfig ("auth-metadata-bare", "[auth]\nmode = \"introspection\"\n[auth.introspection]\nurl = \"https://idp.test/introspect\"\n"));
    CHECK (bare.challengeHeader().empty());
    CHECK (! bare.resourceMetadata().has_value());

    // no config file and an unknown mode mean the token mode
    Authenticator missing (nullptr, "", test::makeTempDir ("auth-metadata-none") / "missing.toml");
    CHECK (missing.external().mode() == AuthMode::token);
    Authenticator unknown (nullptr, "", writeConfig ("auth-metadata-unknown", "[auth]\nmode = \"magic\"\n"));
    CHECK (unknown.external().mode() == AuthMode::token);
    // a broken file fails closed: never the token mode with its native keys and open loopback
    Authenticator broken (nullptr, "", writeConfig ("auth-metadata-broken", "[auth\nmode = "));
    CHECK (broken.external().mode() != AuthMode::token);
    CHECK (! broken.open());
    CHECK (! broken.resolve (AuthRequest {}).ok());
}

ROVOX_TEST (auth, external_modes_never_open_the_server_and_secrets_stay_out_of_errors)
{
    Authenticator proxied (nullptr, "", writeConfig ("auth-closed", proxyToml));
    CHECK (! proxied.open());
    auto r = proxied.resolve (AuthRequest {});
    CHECK (! r.ok());
    CHECK (! test::contains (r.error().message, "proxy-secret"));

    FakeIdp idp;
    idp.introspectBody = R"({"active":false})";
    Authenticator introspected (nullptr, "", writeConfig ("auth-closed-introspection", introspectionToml));
    introspected.external().setFetcher (idp.fetcher());
    auto q = introspected.resolve (bearer ("super-secret-token"));
    CHECK (! q.ok());
    CHECK (! test::contains (q.error().message, "super-secret-token"));
    CHECK (q.error().details.is_null());
}
