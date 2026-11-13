// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Resumable upload storage (task 140), the file side of a tus 1.0 server: creation, offset, streaming append,
// termination, batches, cleanup. Data is appended to `<id>.part` as it arrives (never buffered whole); state lives in a
// small `<id>.json` next to it, so a restart loses nothing. The offset is always taken from the size of the .part file.
#pragma once

#include "Result.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rovox
{

struct UploadLimits
{
    std::uint64_t maxFileBytes = 20ULL << 30;       // 20 GB
    std::uint64_t reserveFreeBytes = 64ULL << 20;   // keep this much free on the volume
    int maxConcurrentPatches = 8;
    std::chrono::seconds ttl = std::chrono::hours (24 * 7);
};

struct UploadInfo
{
    std::string id, filename, relpath, batchId, sha256, tenant = "local";     // sha256: expected hash of the whole file, optional
    std::uint64_t length = 0, offset = 0;
    std::int64_t createdAt = 0, completedAt = 0;             // unix seconds
    std::string placed;                                      // sanitised relpath inside the materialised directory, empty until finish()
    bool complete = false;

    json toJson() const;
};

class UploadStore
{
public:
    /** Callbacks fire after the state change, outside the store's lock. */
    struct Listener
    {
        std::function<void (const UploadInfo&)> onCompleted;
        std::function<void (const std::string& batchId)> onBatchCompleted;
    };

    explicit UploadStore (std::filesystem::path root, UploadLimits = {}, Listener = {});

    /** Every upload belongs to the tenant that created it; for another tenant's upload (or batch) info / terminate / openWriter / batchStatus
        answer NOT_FOUND, exactly as for a missing one. The `tenant` parameters default to `local` (tests, CLI); the HTTP layer always passes the principal's. */
    Result<UploadInfo> create (std::uint64_t length, const std::map<std::string, std::string>& metadata, const std::string& tenant = "local");
    Result<UploadInfo> info (const std::string& id, const std::string& tenant = "local");
    Result<void> terminate (const std::string& id, const std::string& tenant = "local");

    /** One PATCH: open, write the body as it arrives, then finishPatch (or rollback after a bad checksum / dropped connection). */
    class Writer
    {
    public:
        ~Writer();
        bool write (const char* data, size_t size);
        std::uint64_t written() const       { return written_; }
        std::uint64_t startOffset() const   { return start_; }
        /** True once a write was refused because it would pass Upload-Length (the PATCH answers 413). */
        bool overflowed() const             { return overflow_; }
        /** Cuts the file back to where this PATCH started. */
        void rollback();

    private:
        friend class UploadStore;
        UploadStore* store_ = nullptr;
        std::string id_;
        int fd_ = -1;
        std::uint64_t start_ = 0, written_ = 0, length_ = 0;
        bool overflow_ = false;
    };

    /** BUSY if another PATCH on this upload is running (or too many are), PRECONDITION_FAILED if the offset does not match. */
    Result<std::unique_ptr<Writer>> openWriter (const std::string& id, std::uint64_t expectedOffset, const std::string& tenant = "local");
    /** Completes the upload when it reached its length (full-hash check, rename to .done) and tells the listener. */
    Result<UploadInfo> finishPatch (Writer&);

    /** Convenience for tests and the CLI: one whole chunk. */
    Result<UploadInfo> append (const std::string& id, std::uint64_t expectedOffset, const char* data, size_t size, const std::string& tenant = "local");

    /** Batch of files (a .zprj folder). Each entry: {relpath, size, sha256?}. */
    Result<json> createBatch (const json& files, const std::string& tenant = "local");
    /** The result carries `tenant`. A non-empty `tenant` argument restricts the lookup to that tenant (NOT_FOUND otherwise); empty = the operator's view (CLI). */
    Result<json> batchStatus (const std::string& batchId, const std::string& tenant = {});

    /** Materialises a finished batch (`bt_...`) or single upload (`up_...`) as `<root>/<batch_id or upload_id>/<relpath>` by moving the
        .done files (no copy); idempotent. Returns {path, files:[{relpath, bytes, sha256}], detected: zoom_r20|rpp|audio, tenant}. A non-empty `tenant` restricts it to that tenant's uploads (NOT_FOUND otherwise). */
    Result<json> finish (const std::string& batchOrUploadId, const std::string& tenant = {});

    /** Where the files of a batch / upload are materialised (the directory exists only after finish). */
    std::filesystem::path materialisedDir (const std::string& batchOrUploadId, const std::string& tenant = "local") const;

    /** The data dir: with it set, a non-local tenant's uploads are materialised in `<base>/tenants/<tenant>/uploads/<id>`; without it (tests, CLI) everything stays under the store's root. */
    void setTenantBase (std::filesystem::path base) { tenantBase_ = std::move (base); }

    /** Removes unfinished uploads older than the TTL, and finished ones (with their materialised files) older than the TTL after completion. Returns how many. */
    int cleanup (std::int64_t now);

    std::filesystem::path completedPath (const std::string& id) const;
    std::filesystem::path partPath (const std::string& id) const;
    const UploadLimits& limits() const { return limits_; }

private:
    std::filesystem::path jsonPath (const std::string& id) const;
    Result<UploadInfo> load (const std::string& id);
    Result<UploadInfo> load (const std::string& id, const std::string& tenant);      // NOT_FOUND for another tenant's upload
    Result<void> save (const UploadInfo&);
    void releaseWriter (const std::string& id);
    static bool validId (const std::string&);
    std::vector<UploadInfo> membersOf (const std::string& batchOrUploadId);

    std::filesystem::path root_, tenantBase_;
    UploadLimits limits_;
    Listener listener_;
    std::mutex mutex_;
    std::set<std::string> patching_;
};

} // namespace rovox
