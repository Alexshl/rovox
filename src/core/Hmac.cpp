// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Hmac.h"

#include "Sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <vector>

namespace rovox
{

std::string toHex (const std::uint8_t* data, size_t size)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve (size * 2);

    for (size_t i = 0; i < size; ++i)
    {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 15];
    }

    return s;
}

std::array<std::uint8_t, 32> hmacSha256 (const std::string& key, const std::string& message)
{
    std::array<std::uint8_t, 64> block {};

    if (key.size() > 64)
    {
        Sha256 h;
        h.update (key.data(), key.size());
        auto d = h.digest();
        std::copy (d.begin(), d.end(), block.begin());
    }
    else
    {
        std::copy (key.begin(), key.end(), block.begin());
    }

    std::array<std::uint8_t, 64> ipad, opad;

    for (size_t i = 0; i < 64; ++i)
    {
        ipad[i] = block[i] ^ 0x36;
        opad[i] = block[i] ^ 0x5c;
    }

    Sha256 inner;
    inner.update (ipad.data(), ipad.size());
    inner.update (message.data(), message.size());
    const auto innerDigest = inner.digest();

    Sha256 outer;
    outer.update (opad.data(), opad.size());
    outer.update (innerDigest.data(), innerDigest.size());
    return outer.digest();
}

std::string hmacSha256Hex (const std::string& key, const std::string& message)
{
    const auto d = hmacSha256 (key, message);
    return toHex (d.data(), d.size());
}

namespace sigv4
{
namespace
{
std::string keyOf (const std::array<std::uint8_t, 32>& d)  { return std::string (reinterpret_cast<const char*> (d.data()), d.size()); }

std::string trim (const std::string& s)
{
    const auto b = s.find_first_not_of (" \t");
    const auto e = s.find_last_not_of (" \t");
    return b == std::string::npos ? std::string() : s.substr (b, e - b + 1);
}

std::string date8Of (const std::string& amz)    { return amz.substr (0, 8); }

std::string scopeOf (const Credentials& c, const std::string& amz)
{
    return date8Of (amz) + "/" + c.region + "/" + c.service + "/aws4_request";
}

std::string canonicalQuery (const std::map<std::string, std::string>& q)
{
    std::vector<std::pair<std::string, std::string>> pairs;

    for (auto& [k, v] : q)
        pairs.emplace_back (uriEncode (k, false), uriEncode (v, false));

    std::sort (pairs.begin(), pairs.end());
    std::string s;

    for (auto& [k, v] : pairs)
        s += (s.empty() ? "" : "&") + k + "=" + v;

    return s;
}

std::string signedHeaderNames (const std::map<std::string, std::string>& h)
{
    std::string s;

    for (auto& [k, v] : h)
        s += (s.empty() ? "" : ";") + k;

    return s;
}

std::string stringToSign (const Credentials& c, const std::string& amz, const std::string& canonical)
{
    return "AWS4-HMAC-SHA256\n" + amz + "\n" + scopeOf (c, amz) + "\n" + sha256Hex (canonical);
}

std::string signature (const Credentials& c, const std::string& amz, const std::string& canonical)
{
    const auto key = signingKey (c.secretKey, date8Of (amz), c.region, c.service);
    return hmacSha256Hex (keyOf (key), stringToSign (c, amz, canonical));
}
} // namespace

std::string amzDate (std::int64_t unixSeconds)
{
    std::time_t t = (std::time_t) unixSeconds;
    std::tm tm {};
    gmtime_r (&t, &tm);
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

std::string uriEncode (const std::string& s, bool keepSlash)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;

    for (unsigned char c : s)
    {
        if (std::isalnum (c) || c == '-' || c == '_' || c == '.' || c == '~' || (c == '/' && keepSlash))
        {
            out += (char) c;
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }

    return out;
}

std::array<std::uint8_t, 32> signingKey (const std::string& secretKey, const std::string& date8, const std::string& region, const std::string& service)
{
    const auto kDate = hmacSha256 ("AWS4" + secretKey, date8);
    const auto kRegion = hmacSha256 (keyOf (kDate), region);
    const auto kService = hmacSha256 (keyOf (kRegion), service);
    return hmacSha256 (keyOf (kService), "aws4_request");
}

std::string canonicalRequest (const Request& r, const std::map<std::string, std::string>& headers)
{
    std::string canonicalHeaders;

    for (auto& [k, v] : headers)
        canonicalHeaders += k + ":" + trim (v) + "\n";

    return r.method + "\n" + uriEncode (r.path, true) + "\n" + canonicalQuery (r.query) + "\n" + canonicalHeaders + "\n" + signedHeaderNames (headers) + "\n" + r.payloadHash;
}

std::string authorize (const Credentials& c, const Request& r, std::int64_t unixSeconds, std::map<std::string, std::string>& signedHeaders)
{
    const auto amz = amzDate (unixSeconds);
    auto headers = r.headers;
    headers["host"] = r.host;
    headers["x-amz-date"] = amz;
    headers["x-amz-content-sha256"] = r.payloadHash;

    if (! c.sessionToken.empty())
        headers["x-amz-security-token"] = c.sessionToken;

    signedHeaders = headers;
    const auto canonical = canonicalRequest (r, headers);
    return "AWS4-HMAC-SHA256 Credential=" + c.accessKey + "/" + scopeOf (c, amz) + ", SignedHeaders=" + signedHeaderNames (headers) + ", Signature=" + signature (c, amz, canonical);
}

std::string presignQuery (const Credentials& c, const Request& r, std::int64_t unixSeconds, int expiresS)
{
    const auto amz = amzDate (unixSeconds);
    Request pr = r;
    pr.payloadHash = "UNSIGNED-PAYLOAD";
    pr.query["X-Amz-Algorithm"] = "AWS4-HMAC-SHA256";
    pr.query["X-Amz-Credential"] = c.accessKey + "/" + scopeOf (c, amz);
    pr.query["X-Amz-Date"] = amz;
    pr.query["X-Amz-Expires"] = std::to_string (expiresS);
    pr.query["X-Amz-SignedHeaders"] = "host";

    if (! c.sessionToken.empty())
        pr.query["X-Amz-Security-Token"] = c.sessionToken;

    const std::map<std::string, std::string> headers { { "host", r.host } };
    const auto sig = signature (c, amz, canonicalRequest (pr, headers));
    return canonicalQuery (pr.query) + "&X-Amz-Signature=" + sig;
}
} // namespace sigv4

} // namespace rovox
