// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ExternalAuth.h"

#include "Hmac.h"
#include "Ids.h"
#include "Sha256.h"
#include "Url.h"

#include <httplib.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <toml++/toml.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace rovox
{

namespace
{
constexpr std::int64_t positiveTtlS = 60, negativeTtlS = 5, jwksTtlS = 300, jwksRetryS = 10;
constexpr size_t maxCacheEntries = 4096;
constexpr int httpTimeoutS = 3;

Error unauthorized()
{
    return makeError (errc::unauthorized, "missing or invalid credentials");
}

std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

std::string headerOf (const AuthRequest& req, const char* name)
{
    const auto it = req.headers.find (name);
    return it == req.headers.end() ? std::string() : it->second;
}

//==============================================================================
std::optional<std::string> base64urlDecode (std::string_view s)
{
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;

    for (char c : s)
    {
        int v;

        if (c >= 'A' && c <= 'Z')       v = c - 'A';
        else if (c >= 'a' && c <= 'z')  v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')  v = c - '0' + 52;
        else if (c == '-' || c == '+')  v = 62;
        else if (c == '_' || c == '/')  v = 63;
        else if (c == '=')              break;
        else                            return std::nullopt;

        acc = (acc << 6) | (std::uint32_t) v;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            out += (char) ((acc >> bits) & 0xff);
        }
    }

    return out;
}

std::string base64Encode (std::string_view in)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;

    for (size_t i = 0; i < in.size(); i += 3)
    {
        const size_t n = std::min<size_t> (3, in.size() - i);
        std::uint32_t v = (std::uint32_t) (unsigned char) in[i] << 16;

        if (n > 1)  v |= (std::uint32_t) (unsigned char) in[i + 1] << 8;
        if (n > 2)  v |= (unsigned char) in[i + 2];

        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += n > 1 ? alphabet[(v >> 6) & 63] : '=';
        out += n > 2 ? alphabet[v & 63] : '=';
    }

    return out;
}

std::vector<std::string> split (std::string_view s, std::string_view separators)
{
    std::vector<std::string> out;
    std::string cur;

    for (char c : s)
    {
        if (separators.find (c) != std::string_view::npos)
        {
            if (! cur.empty())
                out.push_back (std::move (cur));

            cur.clear();
        }
        else
            cur += c;
    }

    if (! cur.empty())
        out.push_back (std::move (cur));

    return out;
}

//==============================================================================
// DER, just enough to wrap a JWK into a SubjectPublicKeyInfo (no deprecated RSA_* / EC_KEY_* calls)
std::string derTlv (char tag, const std::string& value)
{
    std::string out (1, tag);
    const auto n = value.size();

    if (n < 0x80)
        out += (char) n;
    else
    {
        std::string len;

        for (auto v = n; v > 0; v >>= 8)
            len.insert (len.begin(), (char) (v & 0xff));

        out += (char) (0x80 | len.size());
        out += len;
    }

    return out + value;
}

std::string derInteger (std::string bytes)
{
    size_t skip = 0;

    while (skip + 1 < bytes.size() && bytes[skip] == 0)
        ++skip;

    bytes.erase (0, skip);

    if (bytes.empty())
        bytes = std::string (1, '\0');

    if ((unsigned char) bytes[0] & 0x80)
        bytes.insert (bytes.begin(), '\0');

    return derTlv (0x02, bytes);
}

using KeyPtr = std::shared_ptr<EVP_PKEY>;

KeyPtr keyFromSpki (const std::string& der)
{
    const auto* p = reinterpret_cast<const unsigned char*> (der.data());
    EVP_PKEY* k = d2i_PUBKEY (nullptr, &p, (long) der.size());
    return KeyPtr (k, [] (EVP_PKEY* x) { if (x != nullptr) EVP_PKEY_free (x); });
}

std::string jsonString (const json& j, const char* name)
{
    const auto it = j.find (name);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

//==============================================================================
struct ExternalAuth::Jwk
{
    std::string kty;        // "RSA" | "EC"
    KeyPtr key;
};

namespace
{
struct ClaimNames
{
    std::string tenant = "tenant", subject = "sub", scope = "scope", client = "client_id";
};
} // namespace

struct ExternalAuth::Config
{
    AuthMode mode = AuthMode::token;

    std::vector<Cidr> allow;
    std::string proxySecret;
    std::int64_t maxSkewS = 60;
    std::int64_t leewayS = 60;      // clock skew tolerated on JWT / introspection exp and nbf
    bool allowLocal = false;        // an external identity may carry the tenant `local` (the operator)

    std::string url, clientId, clientSecret, jwksUrl, issuer, audience, resource;
    ClaimNames claims;
};

struct ExternalAuth::Outcome
{
    std::optional<Principal> principal;     // nullopt = rejected
    std::int64_t ttl = 0;
    bool cache = false;
};

namespace
{
ScopeSet withoutForeignAdmin (ScopeSet s, const std::string& tenant)
{
    if (tenant != "local")
        s.remove (Scope::admin);

    return s;
}

/** Scope names the server knows; foreign ones (`openid`, ...) are skipped. */
ScopeSet knownScopes (const std::vector<std::string>& names)
{
    std::vector<std::string> known;

    for (auto& n : names)
        if (ScopeSet::parse ({ n }).ok())
            known.push_back (n);

    return ScopeSet::parse (known).value();
}

/** The principal of an RFC 7662 answer or JWT claim set; nullopt if there is no usable tenant. */
std::optional<Principal> principalFromClaims (const json& c, const ClaimNames& names, bool allowLocal)
{
    const auto tenant = jsonString (c, names.tenant.c_str());

    if (! isValidTenantId (tenant) || (tenant == "local" && ! allowLocal))
        return std::nullopt;

    std::vector<std::string> scopeNames;

    if (const auto it = c.find (names.scope); it != c.end())
    {
        if (it->is_string())
            scopeNames = split (it->get<std::string>(), " ,");
        else if (it->is_array())
            for (auto& v : *it)
                if (v.is_string())
                    scopeNames.push_back (v.get<std::string>());
    }

    Principal p;
    p.tenant = tenant;
    p.subject = jsonString (c, names.subject.c_str());
    p.client = jsonString (c, names.client.c_str());
    p.scopes = withoutForeignAdmin (knownScopes (scopeNames), tenant);
    p.authMode = "introspection";
    return p;
}

/** Lifetime of a positive cache entry: at most 60 s and not past `exp` (0 = none). */
std::int64_t positiveTtl (std::int64_t exp, std::int64_t now)
{
    return exp > 0 ? std::min (positiveTtlS, exp - now) : positiveTtlS;
}

/** When a long-lived connection of that principal must be dropped: `exp` (plus the leeway), but never later than the cache TTL. */
std::int64_t sessionExpiry (std::int64_t exp, std::int64_t leeway, std::int64_t now)
{
    return exp > 0 ? std::min (exp + leeway, now + positiveTtlS) : now + positiveTtlS;
}

bool audienceMatches (const json& claims, const std::string& audience)
{
    const auto it = claims.find ("aud");

    if (it == claims.end())
        return false;

    if (it->is_string())
        return it->get<std::string>() == audience;

    if (it->is_array())
        for (auto& v : *it)
            if (v.is_string() && v.get<std::string>() == audience)
                return true;

    return false;
}

std::int64_t numericClaim (const json& c, const char* name)
{
    const auto it = c.find (name);
    return it != c.end() && it->is_number() ? (std::int64_t) it->get<double>() : 0;
}

std::string envValue (const std::string& name)
{
    if (name.empty())
        return {};

    const char* v = std::getenv (name.c_str());
    return v != nullptr ? std::string (v) : std::string();
}

/** A JWK as an EVP key: RSA from n/e, EC P-256 from x/y. */
std::shared_ptr<ExternalAuth::Jwk> jwkFrom (const json& jwk)
{
    if (! jwk.is_object())
        return nullptr;

    const auto kty = jsonString (jwk, "kty");
    std::string spki;

    if (kty == "RSA")
    {
        const auto n = base64urlDecode (jsonString (jwk, "n"));
        const auto e = base64urlDecode (jsonString (jwk, "e"));

        if (! n || ! e || n->empty() || e->empty())
            return nullptr;

        const auto algorithm = derTlv (0x30, std::string ("\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x01\x01", 11) + std::string ("\x05\x00", 2));
        const auto rsaKey = derTlv (0x30, derInteger (*n) + derInteger (*e));
        spki = derTlv (0x30, algorithm + derTlv (0x03, std::string (1, '\0') + rsaKey));
    }
    else if (kty == "EC")
    {
        const auto x = base64urlDecode (jsonString (jwk, "x"));
        const auto y = base64urlDecode (jsonString (jwk, "y"));

        if (jsonString (jwk, "crv") != "P-256" || ! x || ! y || x->size() != 32 || y->size() != 32)
            return nullptr;

        const auto algorithm = derTlv (0x30, std::string ("\x06\x07\x2A\x86\x48\xCE\x3D\x02\x01", 9) + std::string ("\x06\x08\x2A\x86\x48\xCE\x3D\x03\x01\x07", 10));
        spki = derTlv (0x30, algorithm + derTlv (0x03, std::string ("\x00\x04", 2) + *x + *y));
    }
    else
        return nullptr;

    auto key = keyFromSpki (spki);

    if (! key)
        return nullptr;

    auto out = std::make_shared<ExternalAuth::Jwk>();
    out->kty = kty;
    out->key = key;
    return out;
}

bool verifySignature (EVP_PKEY* key, const std::string& message, const std::string& signature)
{
    struct Ctx
    {
        EVP_MD_CTX* c = EVP_MD_CTX_new();
        ~Ctx() { EVP_MD_CTX_free (c); }
    } ctx;

    if (ctx.c == nullptr || EVP_DigestVerifyInit (ctx.c, nullptr, EVP_sha256(), nullptr, key) != 1)
        return false;

    return EVP_DigestVerify (ctx.c, reinterpret_cast<const unsigned char*> (signature.data()), signature.size(),
                             reinterpret_cast<const unsigned char*> (message.data()), message.size()) == 1;
}

httplib::Headers headersOf (const std::map<std::string, std::string>& h)
{
    return httplib::Headers (h.begin(), h.end());
}
} // namespace

//==============================================================================
std::optional<Cidr> parseCidr (std::string_view text)
{
    Cidr c;
    const auto slash = text.find ('/');
    const std::string address (text.substr (0, slash));

    if (address.empty())
        return std::nullopt;

    if (inet_pton (AF_INET, address.c_str(), c.addr.data()) == 1)
        c.v6 = false;
    else if (inet_pton (AF_INET6, address.c_str(), c.addr.data()) == 1)
        c.v6 = true;
    else
        return std::nullopt;

    const int maxBits = c.v6 ? 128 : 32;
    c.bits = maxBits;

    if (slash != std::string_view::npos)
    {
        const auto digits = text.substr (slash + 1);

        if (digits.empty() || digits.size() > 3 || ! std::all_of (digits.begin(), digits.end(), [] (char ch) { return ch >= '0' && ch <= '9'; }))
            return std::nullopt;

        c.bits = std::stoi (std::string (digits));

        if (c.bits > maxBits)
            return std::nullopt;
    }

    return c;
}

bool cidrContains (const Cidr& net, std::string_view address)
{
    std::string text (address);

    if (const auto zone = text.find ('%'); zone != std::string::npos)
        text.erase (zone);

    if (text.size() > 2 && text.front() == '[' && text.back() == ']')
        text = text.substr (1, text.size() - 2);

    std::array<std::uint8_t, 16> a {};
    bool v6;

    if (inet_pton (AF_INET, text.c_str(), a.data()) == 1)
        v6 = false;
    else if (inet_pton (AF_INET6, text.c_str(), a.data()) == 1)
    {
        v6 = true;
        static constexpr std::uint8_t mappedPrefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

        if (std::equal (std::begin (mappedPrefix), std::end (mappedPrefix), a.begin()))
        {
            for (int i = 0; i < 4; ++i)
                a[(size_t) i] = a[(size_t) (12 + i)];

            v6 = false;
        }
    }
    else
        return false;

    if (v6 != net.v6)
        return false;

    const int full = net.bits / 8, rest = net.bits % 8;

    for (int i = 0; i < full; ++i)
        if (a[(size_t) i] != net.addr[(size_t) i])
            return false;

    if (rest > 0)
    {
        const auto mask = (std::uint8_t) (0xff << (8 - rest));

        if ((a[(size_t) full] & mask) != (net.addr[(size_t) full] & mask))
            return false;
    }

    return true;
}

//==============================================================================
ExternalAuth::ExternalAuth (std::filesystem::path configFile)
    : file_ (std::move (configFile))
{
}

void ExternalAuth::setFetcher (HttpFetcher f)
{
    std::lock_guard<std::mutex> lock (mutex_);
    fetcher_ = std::move (f);
}

void ExternalAuth::setClock (Clock c)
{
    std::lock_guard<std::mutex> lock (mutex_);
    clock_ = std::move (c);
}

std::int64_t ExternalAuth::now() const
{
    Clock c;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        c = clock_;
    }

    return c ? c() : (std::int64_t) std::time (nullptr);
}

HttpReply ExternalAuth::fetch (const std::string& method, const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) const
{
    HttpFetcher f;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        f = fetcher_;
    }

    if (f)
        return f (method, url, body, headers);

    const auto u = parseUrl (url);

    if (! u.valid)
        return {};

    httplib::Client client (u.origin);
    client.set_connection_timeout (httpTimeoutS, 0);
    client.set_read_timeout (httpTimeoutS, 0);
    client.set_write_timeout (httpTimeoutS, 0);
    const auto path = u.path.empty() ? std::string ("/") : u.path;
    const auto res = method == "POST" ? client.Post (path, headersOf (headers), body, "application/x-www-form-urlencoded")
                                      : client.Get (path, headersOf (headers));

    if (! res)
        return {};

    return { res->status, res->body };
}

std::shared_ptr<const ExternalAuth::Config> ExternalAuth::config() const
{
    std::lock_guard<std::mutex> lock (mutex_);

    if (! config_)
        config_ = std::make_shared<Config>();

    if (file_.empty())
        return config_;

    std::error_code ec;
    const auto t = std::filesystem::last_write_time (file_, ec);
    std::optional<std::filesystem::file_time_type> stamp;

    if (! ec)
        stamp = t;


    if (loaded_ && stamp == mtime_)
        return config_;

    loaded_ = true;
    mtime_ = stamp;

    if (! stamp)
    {
        config_ = std::make_shared<Config>();
        haveGoodConfig_ = false;
        cache_.clear();
        return config_;
    }

    try
    {
        const auto doc = toml::parse_file (file_.string());
        auto cfg = std::make_shared<Config>();

        if (const auto* auth = doc["auth"].as_table())
        {
            const auto mode = lower ((*auth)["mode"].value_or (std::string ("token")));

            if (mode == "trusted_proxy")
                cfg->mode = AuthMode::trustedProxy;
            else if (mode == "introspection")
                cfg->mode = AuthMode::introspection;

            cfg->allowLocal = (*auth)["allow_local"].value_or (false);

            if (const auto* tp = (*auth)["trusted_proxy"].as_table())
            {
                if (const auto* allow = (*tp)["allow"].as_array())
                    for (auto& item : *allow)
                        if (const auto text = item.value<std::string>())
                            if (auto c = parseCidr (*text))
                                cfg->allow.push_back (*c);

                cfg->proxySecret = (*tp)["secret"].value_or (std::string());

                if (const auto ref = (*tp)["secret_ref"].value_or (std::string()); ! ref.empty())
                    cfg->proxySecret = envValue (ref);

                cfg->maxSkewS = (*tp)["max_skew_s"].value_or ((std::int64_t) 60);
            }

            if (const auto* in = (*auth)["introspection"].as_table())
            {
                auto str = [&] (const char* k) { return (*in)[k].value_or (std::string()); };
                cfg->url = str ("url");
                cfg->clientId = str ("client_id");
                cfg->clientSecret = str ("client_secret");

                if (const auto ref = str ("client_secret_ref"); ! ref.empty())
                    cfg->clientSecret = envValue (ref);

                cfg->jwksUrl = str ("jwks_url");
                cfg->issuer = str ("issuer");
                cfg->audience = str ("audience");
                cfg->resource = str ("resource");
                cfg->leewayS = std::max<std::int64_t> (0, (*in)["leeway_s"].value_or ((std::int64_t) 60));

                if (const auto v = str ("tenant_claim"); ! v.empty())   cfg->claims.tenant = v;
                if (const auto v = str ("subject_claim"); ! v.empty())  cfg->claims.subject = v;
                if (const auto v = str ("scope_claim"); ! v.empty())    cfg->claims.scope = v;
                if (const auto v = str ("client_claim"); ! v.empty())   cfg->claims.client = v;
            }
        }

        config_ = cfg;
        haveGoodConfig_ = true;
        cache_.clear();
    }
    catch (const toml::parse_error&)
    {
        // an unreadable file keeps the last good configuration. With none yet it fails closed: an external mode with nothing
        // configured rejects every request (never a fallback to native keys or the open loopback). Nothing of the file is logged.
        if (! haveGoodConfig_)
        {
            auto closed = std::make_shared<Config>();
            closed->mode = AuthMode::introspection;
            config_ = closed;
            cache_.clear();
            std::fprintf (stderr, "rovox: warning: %s is not valid TOML: all external requests are rejected until it is fixed\n", file_.filename().string().c_str());
        }
        else
            std::fprintf (stderr, "rovox: warning: %s is not valid TOML: keeping the previous configuration\n", file_.filename().string().c_str());
    }

    return config_;
}

AuthMode ExternalAuth::mode() const
{
    return config()->mode;
}

//==============================================================================
std::optional<Result<Principal>> ExternalAuth::fromProxy (const AuthRequest& req) const
{
    const auto cfg = config();

    if (cfg->mode != AuthMode::trustedProxy)
        return std::nullopt;

    const auto signature = headerOf (req, "x-rovox-signature");
    const auto tenant = headerOf (req, "x-rovox-tenant");

    if (signature.empty() && tenant.empty())
        return std::nullopt;

    const bool fromProxyAddress = std::any_of (cfg->allow.begin(), cfg->allow.end(), [&] (const Cidr& c) { return cidrContains (c, req.remoteAddr); });

    if (! fromProxyAddress)
        return std::nullopt;        // X-Rovox-* from anywhere else is not a credential: treated as if absent

    if (cfg->proxySecret.empty() || signature.empty())
        return Result<Principal> (unauthorized());

    const auto ts = headerOf (req, "x-rovox-ts");
    std::int64_t stamp = 0;

    try { stamp = std::stoll (ts); }
    catch (...) { return Result<Principal> (unauthorized()); }

    const auto skew = now() - stamp;

    if (skew > cfg->maxSkewS || -skew > cfg->maxSkewS)
        return Result<Principal> (unauthorized());

    const auto subject = headerOf (req, "x-rovox-subject");
    const auto scopes = headerOf (req, "x-rovox-scopes");
    const auto client = headerOf (req, "x-rovox-client");
    const auto& target = req.target.empty() ? req.path : req.target;
    const auto expected = hmacSha256Hex (cfg->proxySecret, ts + "\n" + req.method + "\n" + target + "\n" + tenant + "\n" + subject + "\n" + scopes + "\n" + client);

    if (! constantTimeEquals (lower (signature), expected) || ! isValidTenantId (tenant) || (tenant == "local" && ! cfg->allowLocal))
        return Result<Principal> (unauthorized());

    auto parsed = ScopeSet::parse (split (scopes, ", "));

    if (! parsed.ok())
        return Result<Principal> (unauthorized());

    Principal p;
    p.tenant = tenant;
    p.subject = subject;
    p.client = client;
    p.scopes = withoutForeignAdmin (parsed.value(), tenant);
    p.authMode = "trusted_proxy";
    p.expiresAt = now() + positiveTtlS;     // request-scoped: a WS opened through the proxy is re-authenticated after a minute
    return Result<Principal> (p);
}

//==============================================================================
Result<Principal> ExternalAuth::fromBearer (const std::string& token) const
{
    const auto cfg = config();

    if (cfg->mode != AuthMode::introspection || token.empty())
        return unauthorized();

    const auto t = now();
    const auto key = sha256Hex (token);

    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (const auto it = cache_.find (key); it != cache_.end() && it->second.expires > t)
        {
            if (it->second.principal)
                return *it->second.principal;

            return unauthorized();
        }
    }

    const bool jwtShaped = std::count (token.begin(), token.end(), '.') == 2;
    Outcome out;

    if (! cfg->jwksUrl.empty() && jwtShaped)
        out = verifyJwt (*cfg, token, t);
    else if (! cfg->url.empty())
        out = introspect (*cfg, token, t);

    if (out.cache && out.ttl > 0)
    {
        std::lock_guard<std::mutex> lock (mutex_);

        if (cache_.size() >= maxCacheEntries)
        {
            for (auto it = cache_.begin(); it != cache_.end();)
                it = it->second.expires <= t ? cache_.erase (it) : std::next (it);

            if (cache_.size() >= maxCacheEntries)
                cache_.clear();
        }

        cache_[key] = CacheEntry { out.principal, t + out.ttl };
    }

    if (out.principal)
        return *out.principal;

    return unauthorized();
}

ExternalAuth::Outcome ExternalAuth::introspect (const Config& cfg, const std::string& token, std::int64_t t) const
{
    std::map<std::string, std::string> headers { { "Accept", "application/json" } };

    if (! cfg.clientId.empty())
        headers["Authorization"] = "Basic " + base64Encode (cfg.clientId + ":" + cfg.clientSecret);

    const auto reply = fetch ("POST", cfg.url, "token=" + sigv4::uriEncode (token, false) + "&token_type_hint=access_token", headers);

    // no answer or an endpoint failure: fail closed, and do not cache (the token may be fine)
    if (reply.status != 200)
        return {};

    const auto claims = json::parse (reply.body, nullptr, false);

    if (! claims.is_object())
        return {};

    Outcome rejected;
    rejected.cache = true;
    rejected.ttl = negativeTtlS;

    const auto active = claims.find ("active");

    if (active == claims.end() || ! active->is_boolean() || ! active->get<bool>())
        return rejected;

    const auto exp = numericClaim (claims, "exp");

    if (exp > 0 && exp + cfg.leewayS <= t)
        return rejected;

    auto p = principalFromClaims (claims, cfg.claims, cfg.allowLocal);

    if (! p)
        return rejected;

    p->expiresAt = sessionExpiry (exp, cfg.leewayS, t);

    Outcome ok;
    ok.principal = p;
    ok.cache = true;
    ok.ttl = positiveTtl (exp, t);
    return ok;
}

//==============================================================================
std::shared_ptr<const ExternalAuth::Jwk> ExternalAuth::findKey (const Config& cfg, const std::string& kid, std::int64_t t, bool& fetchFailed) const
{
    std::lock_guard<std::mutex> lock (jwksMutex_);
    fetchFailed = false;

    auto lookup = [&]() -> std::shared_ptr<const Jwk>
    {
        if (const auto it = jwks_.find (kid); it != jwks_.end())
            return it->second;

        if (kid.empty() && jwks_.size() == 1)
            return jwks_.begin()->second;

        return nullptr;
    };

    const bool stale = ! jwksLoaded_ || t - jwksFetchedAt_ >= jwksTtlS;

    if (! stale)
        if (auto k = lookup())
            return k;

    // a refresh on every unknown kid would let anyone make us hammer the issuer: at most one per jwksRetryS
    if (! stale && t - jwksFetchedAt_ < jwksRetryS)
        return nullptr;

    const auto reply = fetch ("GET", cfg.jwksUrl, {}, { { "Accept", "application/json" } });
    const auto doc = reply.status == 200 ? json::parse (reply.body, nullptr, false) : json();

    if (! doc.is_object() || ! doc.contains ("keys") || ! doc["keys"].is_array())
    {
        fetchFailed = true;
        return stale ? nullptr : lookup();
    }

    std::map<std::string, std::shared_ptr<const Jwk>> fresh;

    for (auto& jwk : doc["keys"])
        if (auto k = jwkFrom (jwk))
            fresh[jsonString (jwk, "kid")] = k;

    jwks_ = std::move (fresh);
    jwksFetchedAt_ = t;
    jwksLoaded_ = true;
    return lookup();
}

ExternalAuth::Outcome ExternalAuth::verifyJwt (const Config& cfg, const std::string& token, std::int64_t t) const
{
    Outcome rejected;
    rejected.cache = true;
    rejected.ttl = negativeTtlS;

    if (cfg.issuer.empty() || cfg.audience.empty())
        return rejected;                    // a JWT without a pinned issuer and audience is not accepted

    const auto parts = [&]
    {
        std::vector<std::string> v;
        size_t pos = 0;

        for (int i = 0; i < 3; ++i)
        {
            const auto dot = token.find ('.', pos);
            v.push_back (token.substr (pos, dot == std::string::npos ? std::string::npos : dot - pos));
            pos = dot == std::string::npos ? token.size() : dot + 1;
        }

        return v;
    }();

    const auto headerText = base64urlDecode (parts[0]);
    const auto header = headerText ? json::parse (*headerText, nullptr, false) : json();

    if (! header.is_object())
        return rejected;

    const auto alg = jsonString (header, "alg");

    if (alg != "RS256" && alg != "ES256")   // `none`, HS* (a public key must never be used as an HMAC secret) and the rest
        return rejected;

    bool fetchFailed = false;
    const auto key = findKey (cfg, jsonString (header, "kid"), t, fetchFailed);

    if (! key)
    {
        if (fetchFailed)
            return {};                      // the issuer was unreachable: fail closed without caching

        return rejected;
    }

    if ((alg == "RS256") != (key->kty == "RSA"))
        return rejected;

    auto signature = base64urlDecode (parts[2]);

    if (! signature)
        return rejected;

    if (alg == "ES256")
    {
        if (signature->size() != 64)
            return rejected;

        // raw r || s -> DER SEQUENCE { INTEGER r, INTEGER s }
        signature = derTlv (0x30, derInteger (signature->substr (0, 32)) + derInteger (signature->substr (32)));
    }

    if (! verifySignature (key->key.get(), parts[0] + "." + parts[1], *signature))
        return rejected;

    const auto payload = base64urlDecode (parts[1]);
    const auto claims = payload ? json::parse (*payload, nullptr, false) : json();

    if (! claims.is_object())
        return rejected;

    const auto exp = numericClaim (claims, "exp");

    if (jsonString (claims, "iss") != cfg.issuer || ! audienceMatches (claims, cfg.audience) || exp + cfg.leewayS <= t)
        return rejected;

    if (claims.contains ("nbf") && numericClaim (claims, "nbf") > t + cfg.leewayS)
        return {};                          // not valid yet: reject, but do not remember it

    auto p = principalFromClaims (claims, cfg.claims, cfg.allowLocal);

    if (! p)
        return rejected;

    p->expiresAt = sessionExpiry (exp, cfg.leewayS, t);

    Outcome ok;
    ok.principal = p;
    ok.cache = true;
    ok.ttl = positiveTtl (exp, t);
    return ok;
}

//==============================================================================
std::string ExternalAuth::challengeHeader() const
{
    const auto cfg = config();

    if (cfg->mode != AuthMode::introspection || cfg->resource.empty())
        return {};

    auto base = cfg->resource;

    while (! base.empty() && base.back() == '/')
        base.pop_back();

    return "Bearer resource_metadata=\"" + base + "/.well-known/oauth-protected-resource\"";
}

std::optional<json> ExternalAuth::resourceMetadata() const
{
    const auto cfg = config();

    if (cfg->mode != AuthMode::introspection || cfg->resource.empty())
        return std::nullopt;

    json j { { "resource", cfg->resource }, { "bearer_methods_supported", json::array ({ "header" }) } };

    if (! cfg->issuer.empty())
        j["authorization_servers"] = json::array ({ cfg->issuer });

    return j;
}

} // namespace rovox
