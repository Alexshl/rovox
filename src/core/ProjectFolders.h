// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Project folders (E12): one small document per tenant that maps project ids to folders. It lives next to the projects, never inside a project:
// moving a project changes only this document (no project revision, no lease, no undo step).
//
//   bucket:     <prefix>tenants/<tenant>/folders.json         (StorageKeys::folders)
//   local mode: <projects root>/../folders.json               (also the local copy in S3 mode, read when the bucket cannot be reached)
//
// Schema 1: {schema, version, updated_at, folders: [{folder_id, name, parent_id, created_at, updated_at}], projects: {p_...: fld_... | "tests"}}.
// A project missing from `projects` is at the top level. The system folder `tests` ("Тестові треки") is never stored: every read adds it.
#pragma once

#include "RemoteStorage.h"

#include <chrono>
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

struct Folder
{
    std::string id, name;
    std::string parentId;               // empty = top level
    std::string createdAt, updatedAt;
    bool system = false;
};

struct FoldersDoc
{
    std::uint64_t version = 0;
    std::string updatedAt;
    std::vector<Folder> folders;                        // the stored folders (never `tests`)
    std::map<std::string, std::string> projects;        // project id -> folder id

    json toJson() const;
    /** Tolerant: entries that are not valid (bad ids, a stored `tests`, duplicates) are dropped. */
    static FoldersDoc fromJson (const json&);

    /** A stored folder or the system folder `tests`; nullptr if unknown. */
    const Folder* find (const std::string& folderId) const;
    /** Folder of a project; "" = top level (also when the assignment points at a folder that no longer exists). */
    std::string folderOf (const std::string& projectId) const;
    /** `folderId` and every folder below it. */
    std::set<std::string> subtree (const std::string& folderId) const;
    /** 1 for a top-level folder. */
    int depthOf (const std::string& folderId) const;
};

/** Which projects of a tenant exist right now. `complete` = false when the list may be missing some (the bucket's heads could not be listed):
    then assignments of projects not in `ids` are kept instead of cleaned up. */
struct KnownProjects
{
    std::set<std::string> ids;
    bool complete = true;
};

/** Reads and changes the folders document of a tenant. Backed by a local file, or by an object store (with that file as the local copy).
    Changes are read-modify-write: conditional puts (If-Match / If-None-Match: *) when the store enforces them, else a put plus a read-back,
    retried up to 5 times. Reads are cached per tenant for 10 s, then revalidated with the ETag. Thread-safe (one call at a time). */
class ProjectFolders
{
public:
    static constexpr const char* rootId = "root";
    static constexpr const char* testsId = "tests";
    static constexpr const char* testsName = "Тестові треки";
    static constexpr int maxDepth = 8;
    static constexpr int maxFolders = 1000;
    static constexpr int maxNameLength = 100;
    static constexpr int maxAttempts = 5;

    using LocalPath = std::function<std::filesystem::path (const std::string& tenant)>;
    using KnownProjectsFn = std::function<KnownProjects (const std::string& tenant)>;

    explicit ProjectFolders (LocalPath);

    /** nullptr store = local mode. `conditional` says whether the store enforces preconditions (RemoteStorage::conditionalWritesActive). */
    void setStore (std::shared_ptr<ObjectStore>, StorageKeys, std::function<bool()> conditional);
    void setKnownProjects (KnownProjectsFn f)                   { known_ = std::move (f); }
    void setCacheTtl (std::chrono::milliseconds ttl)            { cacheTtl_ = ttl; }

    /** The document (never fails: an unreachable bucket gives the last cached or local copy, a missing one an empty document). */
    FoldersDoc read (const std::string& tenant);

    struct Change
    {
        std::uint64_t version = 0;
        Folder folder;
    };

    /** `parentId` "" / "root" = top level. NOT_FOUND (parent), INVALID_PARAMS (name, duplicate, depth, parent `tests`), QUOTA_EXCEEDED (> 1000). */
    Result<Change> create (const std::string& tenant, const std::string& name, const std::string& parentId);
    /** PRECONDITION_FAILED on `tests`, NOT_FOUND, INVALID_PARAMS (name, duplicate). */
    Result<Change> rename (const std::string& tenant, const std::string& folderId, const std::string& name);
    /** INVALID_PARAMS (cycle, depth, duplicate, parent `tests`), PRECONDITION_FAILED on `tests`, NOT_FOUND. */
    Result<Change> move (const std::string& tenant, const std::string& folderId, const std::string& parentId);
    /** Only an empty folder (no subfolders, no existing projects): PRECONDITION_FAILED {projects, folders} otherwise, and on `tests`. */
    Result<Change> remove (const std::string& tenant, const std::string& folderId);
    /** Puts projects into a folder ("" / "root" = top level, "tests" allowed). The caller checks that the projects exist. NOT_FOUND (folder). */
    Result<Change> assign (const std::string& tenant, const std::vector<std::string>& projectIds, const std::string& folderId);
    /** Drops a deleted project's assignment. Best effort: failures are logged, never returned. */
    void forget (const std::string& tenant, const std::string& projectId);

    /** API form of the folders: stored ones sorted by name, then `tests`; each with system, project_count (existing projects only), folder_count. */
    static json foldersJson (const FoldersDoc&, const std::set<std::string>& existingProjects);
    static json folderJson (const Folder&, const FoldersDoc&, const std::set<std::string>& existingProjects);

    /** Trims and checks a folder name; the trimmed name or INVALID_PARAMS. */
    static Result<std::string> normaliseName (const std::string&);
    /** Case-insensitive key of a name (ASCII, Latin-1, Greek and Cyrillic letters folded). */
    static std::string foldCase (const std::string&);

private:
    using Mutate = std::function<Result<void> (FoldersDoc&)>;
    Result<FoldersDoc> update (const std::string& tenant, const Mutate&);
    Result<FoldersDoc> updateLocal (const std::string& tenant, const Mutate&);
    Result<FoldersDoc> updateRemote (const std::string& tenant, const Mutate&);
    /** Version, timestamp and the clean-up of assignments that point nowhere. */
    void finish (const std::string& tenant, FoldersDoc&, std::uint64_t previousVersion);

    FoldersDoc readLocal (const std::string& tenant) const;
    Result<void> writeLocal (const std::string& tenant, const std::string& body) const;
    std::string key (const std::string& tenant) const;

    struct CacheEntry
    {
        FoldersDoc doc;
        std::string etag;
        std::chrono::steady_clock::time_point fetched {};
    };

    LocalPath localPath_;
    KnownProjectsFn known_;
    std::shared_ptr<ObjectStore> store_;
    StorageKeys keys_;
    std::function<bool()> conditional_;
    std::chrono::milliseconds cacheTtl_ { 10000 };
    std::map<std::string, CacheEntry> cache_;
    std::recursive_mutex mutex_;
};

} // namespace rovox
