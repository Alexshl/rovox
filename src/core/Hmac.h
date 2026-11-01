// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// HMAC-SHA256 (RFC 2104) and AWS Signature Version 4 (request signing and presigned URLs for S3-compatible storage).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace rovox
{

std::array<std::uint8_t, 32> hmacSha256 (const std::string& key, const std::string& message);
std::string hmacSha256Hex (const std::string& key, const std::string& message);
std::string toHex (const std::uint8_t* data, size_t size);

namespace sigv4
{
struct Credentials
{
    std::string accessKey, secretKey, sessionToken;     // sessionToken may be empty
    std::string region = "us-east-1", service = "s3";
};

/** "20150830T123600Z" for a unix time. */
std::string amzDate (std::int64_t unixSeconds);

/** RFC 3986 percent-encoding as AWS wants it ('/' kept when `keepSlash`). */
std::string uriEncode (const std::string&, bool keepSlash);

std::array<std::uint8_t, 32> signingKey (const std::string& secretKey, const std::string& date8, const std::string& region, const std::string& service);

struct Request
{
    std::string method = "GET";
    std::string host;                                   // as sent in the Host header (with port if not default)
    std::string path = "/";                             // already the URI path, will be encoded by the signer
    std::map<std::string, std::string> query;           // decoded names and values
    std::map<std::string, std::string> headers;         // extra headers to sign (lower-case names); host and x-amz-* are added
    std::string payloadHash = "UNSIGNED-PAYLOAD";       // hex sha256 of the body, or UNSIGNED-PAYLOAD
};

/** Value of the Authorization header; also fills x-amz-date / x-amz-content-sha256 (and x-amz-security-token) into `signedHeaders`. */
std::string authorize (const Credentials&, const Request&, std::int64_t unixSeconds, std::map<std::string, std::string>& signedHeaders);

/** Canonical request string (exposed for tests against the AWS documentation). */
std::string canonicalRequest (const Request&, const std::map<std::string, std::string>& allHeaders);

/** Query string of a presigned GET URL valid for `expiresS` (X-Amz-Algorithm ... X-Amz-Signature). The host is signed. */
std::string presignQuery (const Credentials&, const Request&, std::int64_t unixSeconds, int expiresS);
} // namespace sigv4

} // namespace rovox
