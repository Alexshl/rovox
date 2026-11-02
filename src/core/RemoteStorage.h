// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Object storage (S3-compatible, e.g. Hetzner Object Storage) as the source of truth for projects, media and renders. The server keeps a
// working cache under the data directory; RemoteStorage mirrors it to the bucket:
//
//   <prefix>tenants/<tenant>/projects/<id>/rev/<revision>/{edit.xml,rovox.json}   immutable snapshots
//   <prefix>tenants/<tenant>/projects/<id>/HEAD                                   {revision,name,modified,tracks,duration_s}
//   <prefix>tenants/<tenant>/projects/<id>/LEASE                                  {owner,expires}: one writer per project
//   <prefix>tenants/<tenant>/projects/<id>/renders/<file>
//   <prefix>tenants/<tenant>/media/sha256/<ab>/<hash>.<ext>                       content-addressed, uploaded once
//
// Nothing here may block the message thread or the audio thread: snapshots are queued and uploaded by one background thread.
#pragma once

#include "ObjectStore.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace rovox
{

/** Where the working state lives. `local`: only the data directory (default). `s3`: the bucket is the source of truth. */
struct StorageConfig
{
    bool s3 = false;
    S3StoreConfig store;
    std::string prefix = "rovox/";
    int leaseTtlS = 60;

    /** Reads S3_ENDPOINT, S3_REGION, S3_BUCKET, S3_ACCESS_KEY, S3_SECRET_KEY, S3_PREFIX (each also as <NAME>_FILE: first line of that file),
        ROVOX_STORAGE (s3 | local), ROVOX_LEASE_TTL_S. `mode` (the --storage option) overrides ROVOX_STORAGE. Without either, S3 is used when
        S3_ENDPOINT and S3_BUCKET are both set, else local. */
    static Result<StorageConfig> fromEnvironment (const std::string& mode = "");

    /** For status output: never a key. */
    json describe() const;
};

/** Key layout of the bucket. */
struct StorageKeys
{
    std::string prefix;     // "" or ends with '/'

    static std::string normalisePrefix (std::string);
    std::string tenantsPrefix() const                                                       { return prefix + "tenants/"; }
    std::string projectsPrefix (const std::string& tenant) const                            { return tenantsPrefix() + tenant + "/projects/"; }
    std::string projectRoot (const std::string& tenant, const std::string& id) const        { return projectsPrefix (tenant) + id + "/"; }
    std::string head (const std::string& tenant, const std::string& id) const               { return projectRoot (tenant, id) + "HEAD"; }
    std::string lease (const std::string& tenant, const std::string& id) const              { return projectRoot (tenant, id) + "LEASE"; }
    std::string revFile (const std::string& tenant, const std::string& id, std::uint64_t revision, const std::string& file) const
                                                                                            { return projectRoot (tenant, id) + "rev/" + std::to_string (revision) + "/" + file; }
    std::string render (const std::string& tenant, const std::string& id, const std::string& file) const
                                                                                            { return projectRoot (tenant, id) + "renders/" + file; }
    /** `ext` with the dot (".wav") or empty. */
    std::string media (const std::string& tenant, const std::string& sha256, const std::string& ext) const
                                                                                            { return tenantsPrefix() + tenant + "/media/sha256/" + sha256.substr (0, 2) + "/" + sha256 + ext; }
};

/** The small JSON object next to a project's snapshots: what project.catalog needs without reading edit.xml. */
struct ProjectHead
{
    std::string tenant = "local", projectId, name;
    std::uint64_t revision = 0;
    std::int64_t modified = 0;      // unix seconds
    int tracks = 0;
    double durationS = 0;

    json toJson() const;
    static std::optional<ProjectHead> fromJson (const json&, const std::string& tenant, const std::string& projectId);
};

struct LeaseInfo
{
    std::string owner;
    std::int64_t expires = 0;       // unix seconds
    std::int64_t acquired = 0;
};

/** One writer per project. Not a hard lock (S3 has no portable compare-and-swap): the holder writes LEASE with an expiry and renews it;
    others refuse while it is live; a verification read after writing narrows the race. */
class ProjectLease
{
public:
    using Clock = std::function<std::int64_t()>;
    static std::int64_t systemNow();

    /** Empty optional = no lease object (or an unreadable one); an Error = the store could not be asked. */
    static Result<std::optional<LeaseInfo>> read (ObjectStore&, const std::string& key);
    /** BUSY (with details owner / expires) while another owner's lease is live. */
    static Result<LeaseInfo> acquire (ObjectStore&, const std::string& key, const std::string& owner, int ttlS, const Clock& = systemNow);
    /** Extends our lease; BUSY if somebody else took it over. */
    static Result<LeaseInfo> renew (ObjectStore&, const std::string& key, const std::string& owner, int ttlS, const Clock& = systemNow);
    static void release (ObjectStore&, const std::string& key, const std::string& owner);
};

struct MediaRef
{
    std::string sha256, relPath;        // relPath is "media/<sha256>.<ext>" below the project directory
};

/** Everything one upload needs, captured on the message thread right after the local atomic write. */
struct SnapshotJob
{
    std::string tenant, projectId;
    std::uint64_t revision = 0;
    std::string editXml, manifestJson;
    ProjectHead head;
    std::vector<MediaRef> media;
    std::filesystem::path projectDir;   // where media/ lives (the working cache)
};

/** Reads edit.xml + rovox.json of `dir` (a project directory or an autosave/<rev>.rvx one) into a job. Media lists come from the manifest. */
Result<SnapshotJob> readSnapshot (const std::filesystem::path& dir, const std::filesystem::path& projectDir, ProjectHead head);

struct StorageStatus
{
    std::optional<std::uint64_t> syncedRevision;
    int pending = 0;
    std::string lastError;
    bool leaseHeld = false;
};

class RemoteStorage
{
public:
    using StatusListener = std::function<void (const std::string& tenant, const std::string& projectId, const json& status)>;

    RemoteStorage (std::shared_ptr<ObjectStore>, StorageKeys, std::string description, std::string owner, int leaseTtlS = 60);
    ~RemoteStorage();

    /** Starts the uploader thread (lease renewal and head refresh included). */
    void start();
    /** Waits until everything queued is uploaded (or `timeout`), then releases the leases and stops the thread. true = nothing was left. */
    bool shutdown (std::chrono::seconds timeout);
    /** Waits for an empty queue only. */
    bool flush (std::chrono::seconds timeout);

    ObjectStore& store()                    { return *store_; }
    const StorageKeys& keys() const         { return keys_; }
    const std::string& owner() const        { return owner_; }
    json describe() const;
    void setListener (StatusListener);
    /** Test hook: how often the background thread wakes up for lease renewal and head refresh (default 1 s / 30 s / ttl/3). */
    void setIntervals (std::chrono::milliseconds headRefresh, std::chrono::milliseconds leaseRenew);
    void setRetryBackoff (std::chrono::milliseconds base)       { retryBase_ = base; }
    void setClock (ProjectLease::Clock c)                       { clock_ = std::move (c); }

    // ---- writer lease ----
    Result<void> acquireLease (const std::string& tenant, const std::string& projectId);
    /** Released by the uploader thread once the project's queued snapshot is out. */
    void releaseLease (const std::string& tenant, const std::string& projectId);

    // ---- push ----
    /** Latest snapshot per project wins; returns at once. */
    void enqueue (SnapshotJob);
    /** Synchronous: media, then rev/<n>/, then HEAD (only if it moves forward). Used by the uploader thread and `rovox storage push`. */
    Result<void> push (const SnapshotJob&);
    /** Uploads one render result (and its sidecar if present); returns the object key. */
    Result<std::string> uploadRender (const std::string& tenant, const std::string& projectId, const std::filesystem::path& file);
    Result<void> fetchRender (const std::string& tenant, const std::string& projectId, const std::string& name, const std::filesystem::path& dest);
    /** Uploads every finished file of `<projectDir>/renders` the bucket does not have yet (same size). Returns how many went up. */
    Result<int> pushRenders (const std::string& tenant, const std::string& projectId, const std::filesystem::path& projectDir);
    std::vector<std::string> listRenders (const std::string& tenant, const std::string& projectId);
    /** Deletes HEAD and LEASE (snapshots and media stay in the bucket). Queued behind the project's pending snapshot. */
    void forget (const std::string& tenant, const std::string& projectId);

    // ---- pull ----
    Result<ProjectHead> fetchHead (const std::string& tenant, const std::string& projectId);
    /** Downloads the HEAD revision and its missing media into `projectDir` (media first, rovox.json last). Returns the revision. */
    Result<std::uint64_t> pull (const std::string& tenant, const std::string& projectId, const std::filesystem::path& projectDir);
    /** Lists the bucket's tenants and projects into the head cache. */
    Result<void> refreshHeads();
    std::vector<ProjectHead> heads (const std::string& tenant) const;
    std::optional<ProjectHead> cachedHead (const std::string& tenant, const std::string& projectId) const;
    /** Newest project of the tenant with this name. */
    std::optional<ProjectHead> findByName (const std::string& tenant, const std::string& name) const;

    // ---- status ----
    json status (const std::string& tenant, const std::string& projectId) const;
    int pendingUploads() const;

private:
    struct State
    {
        std::optional<std::uint64_t> synced;
        std::string lastError;
        bool leaseHeld = false, leaseLost = false, releaseWanted = false, forgetWanted = false;
        std::int64_t leaseRenewedAt = 0;
        int attempts = 0;
        std::chrono::steady_clock::time_point notBefore {};
    };

    using Key = std::pair<std::string, std::string>;

    void loop();
    bool processOne (bool ignoreBackoff);
    void housekeeping();
    void emit (const Key&);
    json statusLocked (const Key&) const;
    Result<void> uploadMedia (const SnapshotJob&, std::string& warning);
    void setHead (const ProjectHead&);

    std::shared_ptr<ObjectStore> store_;
    StorageKeys keys_;
    std::string description_, owner_;
    int leaseTtlS_;

    mutable std::mutex mutex_;
    std::condition_variable cv_, idleCv_;
    std::map<Key, SnapshotJob> queue_;
    std::map<Key, State> states_;
    std::set<Key> inFlight_, flushTried_;
    std::map<std::string, std::map<std::string, ProjectHead>> heads_;
    std::set<std::string> knownMedia_;
    std::thread thread_;
    bool stop_ = false, started_ = false, flushing_ = false;
    std::chrono::milliseconds headRefresh_ { 30000 }, leaseRenew_ { 0 }, retryBase_ { 2000 };
    std::chrono::steady_clock::time_point lastRefresh_ {}, lastRenew_ {};
    ProjectLease::Clock clock_ = ProjectLease::systemNow;
    StatusListener listener_;
};

} // namespace rovox
