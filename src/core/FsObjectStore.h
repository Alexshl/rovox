// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// An ObjectStore over a local directory (a mounted volume, NFS, a dev box): key "a/b/c" is the file <root>/a/b/c.
//  - Keys are validated: not empty, no leading or trailing '/', no empty, "." or ".." segment, no control characters, no segment that starts with
//    ".rovox-" (lock and temporary files) and no key ending in ".etag" (the sidecar). Anything else is INVALID_PARAMS.
//  - A put writes a temporary file in the target directory (hashing it on the way), fsyncs it and renames it into place, so a reader sees the old
//    or the new object, never a partial one; a crash before the rename leaves only an invisible ".rovox-tmp-*" file.
//  - ETag = "<sha256 of the content>" (quoted, like S3), computed while writing and stored in the sidecar file "<name>.etag" ("<hex> <size> <mtime_ns>").
//    A sidecar that does not match the file (size or mtime differ, or it is missing) is ignored and the hash is recomputed.
//  - Conditional puts (ifNoneMatch "*" / an ETag, ifMatch) are decided under an exclusive flock() on the directory's ".rovox-lock" file, so two
//    processes (or threads) on the same root behave like two clients of one S3 bucket: exactly one of two racing If-None-Match puts wins.
//  - list() is in lexicographic byte order of the keys, like S3.
#pragma once

#include "ObjectStore.h"

#include <atomic>
#include <functional>

namespace rovox
{

class FsObjectStore : public ObjectStore
{
public:
    /** Creates `root` if needed. */
    explicit FsObjectStore (std::filesystem::path root);

    Result<ObjectMeta> head (const std::string& key) override;
    Result<std::string> get (const std::string& key) override;
    Result<void> getFile (const std::string& key, const std::filesystem::path& dest) override;
    Result<void> put (const std::string& key, const std::string& body, const std::string& contentType = "application/octet-stream") override;
    Result<void> putFile (const std::string& key, const std::filesystem::path& file, const std::string& contentType = "application/octet-stream") override;
    Result<void> putIf (const std::string& key, const std::string& body, const std::string& contentType, const PutCondition&) override;
    Result<void> putFileCancellable (const std::string& key, const std::filesystem::path& file, const std::string& contentType, const CancelCheck&) override;
    Result<ListResult> list (const std::string& prefix, const std::string& delimiter = "") override;
    Result<void> remove (const std::string& key) override;
    std::string describe() const override;

    const std::filesystem::path& root() const   { return root_; }

    /** Test seam: called after the temporary file is complete and before the rename; returning true abandons the put as a crash would. */
    void setBeforeCommitHook (std::function<bool()> hook)   { beforeCommit_ = std::move (hook); }

    /** Empty if the key is acceptable, else the reason. */
    static std::string checkKey (const std::string& key);

private:
    struct Source;
    Result<void> store (const std::string& key, const Source&, const PutCondition*, const CancelCheck&);
    std::filesystem::path pathOf (const std::string& key) const;

    std::filesystem::path root_;
    std::function<bool()> beforeCommit_;
    std::atomic<unsigned> counter_ { 0 };
};

std::shared_ptr<ObjectStore> makeFsObjectStore (std::filesystem::path root);

} // namespace rovox
