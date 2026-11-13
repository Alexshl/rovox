// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Job output files and signed download URLs (E8-06).
// A finished job's files live in <data>/tenants/<tenant>/jobs/<job_id>/out/<name>; the job's manifest.json (names, sizes, sha256) is written after the files.
// A signed URL is `/files/<tenant>/<job_id>/<name>?exp=<unix>&sig=<hex>`: sig = HMAC-SHA256(<data>/auth/files.key, "GET|<path>|<exp>|<tenant>").
// The route is public (no bearer): the signature is the credential. It is multi-use until `exp`; a changed tenant, job, name or exp, or an expired link,
// is refused. Only a plain file inside the job's own out/ directory is ever served; a name is [A-Za-z0-9._-] and never starts with a dot.
#pragma once

#include "Result.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace rovox
{

/** A file name that may be published and served: 1..128 characters of [A-Za-z0-9._-], not starting with a dot. */
bool isValidJobFileName (const std::string& name);

class JobFiles
{
public:
    /** `dataDir` is the server data directory; the signing key is <dataDir>/auth/files.key (32 random bytes, 0600, created on first use). */
    explicit JobFiles (std::filesystem::path dataDir);

    /** <data>/tenants/<tenant>/jobs/<job_id>/out; empty for an invalid tenant or job id. */
    std::filesystem::path outDir (const std::string& tenant, const std::string& jobId) const;

    /** Copies `source` into the job's out/ directory as `name` (written to a temporary name, then renamed) and records it in manifest.json,
        which is rewritten last. Idempotent: the same name again replaces the file. Returns {name, bytes, sha256}. */
    Result<json> publish (const std::string& tenant, const std::string& jobId, const std::string& name, const std::filesystem::path& source);

    /** The manifest of a job ({"job_id", "files": [{name, bytes, sha256}]}); NOT_FOUND without one. */
    Result<json> manifest (const std::string& tenant, const std::string& jobId) const;

    /** The path and query of a signed URL, valid until `expiresAt` (unix seconds): "/files/<tenant>/<job_id>/<name>?exp=...&sig=...". Empty for an invalid tenant, job or name. */
    std::string signedPath (const std::string& tenant, const std::string& jobId, const std::string& name, std::int64_t expiresAt);

    /** Checks a request for GET /files/<tenant>/<job_id>/<name> and gives the file to serve:
        UNAUTHORIZED for a bad or expired signature (one answer for both), NOT_FOUND for a valid signature on a missing file. */
    Result<std::filesystem::path> resolve (const std::string& tenant, const std::string& jobId, const std::string& name, const std::string& exp, const std::string& sig, std::int64_t now);

private:
    std::string key();
    std::string mac (const std::string& tenant, const std::string& jobId, const std::string& name, const std::string& exp);

    std::filesystem::path dataDir_;
    std::mutex mutex_;
    std::string key_;
};

} // namespace rovox
