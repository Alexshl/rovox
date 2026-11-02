// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Object storage as the server sees it: a flat key space with put / get / head / list / delete. S3ObjectStore speaks S3 (SigV4, path-style
// or virtual-host addressing, retries, multipart upload); tests use an in-memory fake behind the same interface.
#pragma once

#include "Result.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rovox
{

struct ObjectMeta
{
    std::string key, etag;
    std::uint64_t size = 0;
};

struct ListResult
{
    std::vector<ObjectMeta> objects;
    std::vector<std::string> prefixes;      // "directories" below the prefix, only when a delimiter was given
};

/** Thread-safe: every call may be made from any thread. A missing object is the error code NOT_FOUND. */
class ObjectStore
{
public:
    virtual ~ObjectStore() = default;

    virtual Result<ObjectMeta> head (const std::string& key) = 0;
    virtual Result<std::string> get (const std::string& key) = 0;
    /** Streams to `dest` (written as dest.part, then renamed). */
    virtual Result<void> getFile (const std::string& key, const std::filesystem::path& dest) = 0;
    virtual Result<void> put (const std::string& key, const std::string& body, const std::string& contentType = "application/octet-stream") = 0;
    virtual Result<void> putFile (const std::string& key, const std::filesystem::path& file, const std::string& contentType = "application/octet-stream") = 0;
    /** All keys below `prefix`; with a non-empty `delimiter` the first level is folded into ListResult::prefixes. Follows pagination. */
    virtual Result<ListResult> list (const std::string& prefix, const std::string& delimiter = "") = 0;
    virtual Result<void> remove (const std::string& key) = 0;

    virtual std::string describe() const = 0;   // safe to show: never a secret
};

struct S3StoreConfig
{
    std::string endpoint;                       // https://fsn1.your-objectstorage.com
    std::string region = "us-east-1";
    std::string bucket;
    std::string accessKey, secretKey, sessionToken;
    bool pathStyle = true;                      // https://host/bucket/key (Hetzner, MinIO); false = https://bucket.host/key
    std::uint64_t multipartThreshold = 64ull << 20;     // files above this go up in parts of partSize
    std::uint64_t partSize = 64ull << 20;
    int maxAttempts = 5;                        // per request, for network errors, 5xx and 429
    int backoffBaseMs = 400;                    // 400, 800, 1600 ... plus jitter
    int connectTimeoutS = 10, ioTimeoutS = 300;
};

std::shared_ptr<ObjectStore> makeS3ObjectStore (S3StoreConfig);

} // namespace rovox
