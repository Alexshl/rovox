// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "RemoteStorage.h"

#include "Delivery.h"
#include "Ids.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
std::string lowerCase (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

/** $NAME, else the first line of the file named by $NAME_FILE. */
std::string envValue (const std::string& name)
{
    const char* file = std::getenv ((name + "_FILE").c_str());
    return SecretRef { name, file != nullptr ? file : "" }.resolve();
}

bool isHex64 (const std::string& s)
{
    return s.size() == 64 && std::all_of (s.begin(), s.end(), [] (unsigned char c) { return std::isdigit (c) || (c >= 'a' && c <= 'f'); });
}

/** "media/<sha256>.<ext>" and nothing else: a name from the bucket never leaves the media directory. Returns the extension with its dot. */
std::optional<std::string> mediaExtension (const std::string& sha256, const std::string& relPath)
{
    if (! isHex64 (sha256) || relPath.rfind ("media/", 0) != 0)
        return std::nullopt;

    const auto file = relPath.substr (6);

    if (file.find ('/') != std::string::npos || file.find ("..") != std::string::npos || file.rfind (sha256, 0) != 0)
        return std::nullopt;

    const auto ext = file.substr (64);

    if (! ext.empty() && (ext[0] != '.' || ext.size() > 12 || ! std::all_of (ext.begin() + 1, ext.end(), [] (unsigned char c) { return std::isalnum (c) != 0; })))
        return std::nullopt;

    return lowerCase (ext);
}

std::string contentTypeFor (const fs::path& p)
{
    const auto e = lowerCase (p.extension().string());

    if (e == ".wav")    return "audio/wav";
    if (e == ".flac")   return "audio/flac";
    if (e == ".mp3")    return "audio/mpeg";
    if (e == ".json")   return "application/json";
    if (e == ".xml")    return "application/xml";
    return "application/octet-stream";
}

std::string readWhole (const fs::path& f)
{
    std::ifstream in (f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Result<void> writeAtomic (const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories (path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
        out << content;

        if (! out)
            return makeError (errc::io, "cannot write " + path.string());
    }
    fs::rename (tmp, path, ec);
    return ec ? Result<void> (makeError (errc::io, "cannot write " + path.string() + ": " + ec.message())) : Result<void>();
}

bool safeName (const std::string& n)
{
    return ! n.empty() && n[0] != '.' && n.find ('/') == std::string::npos && n.find ('\\') == std::string::npos && n.find ("..") == std::string::npos;
}

std::string isoTime (std::int64_t t)
{
    std::time_t tt = (std::time_t) t;
    std::tm tm {};
    gmtime_r (&tt, &tm);
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
} // namespace

//==============================================================================
Result<StorageConfig> StorageConfig::fromEnvironment (const std::string& modeOption)
{
    StorageConfig c;
    auto mode = lowerCase (modeOption);

    if (mode.empty())
        if (const char* env = std::getenv ("ROVOX_STORAGE"))
            mode = lowerCase (env);

    if (mode != "" && mode != "local" && mode != "s3")
        return makeError (errc::invalidParams, "storage must be s3 or local (got '" + mode + "')");

    const auto endpoint = envValue ("S3_ENDPOINT"), bucket = envValue ("S3_BUCKET");

    if (mode.empty())
        mode = (! endpoint.empty() && ! bucket.empty()) ? "s3" : "local";

    if (mode == "local")
        return c;

    const auto access = envValue ("S3_ACCESS_KEY"), secret = envValue ("S3_SECRET_KEY");
    std::string missing;

    for (auto& [name, value] : std::vector<std::pair<std::string, std::string>> { { "S3_ENDPOINT", endpoint }, { "S3_BUCKET", bucket }, { "S3_ACCESS_KEY", access }, { "S3_SECRET_KEY", secret } })
        if (value.empty())
            missing += (missing.empty() ? "" : ", ") + name;

    if (! missing.empty())
        return makeError (errc::preconditionFailed, "storage is s3 but these variables are not set (or set <NAME>_FILE to a secret file): " + missing);

    c.s3 = true;
    c.store.endpoint = endpoint;
    c.store.bucket = bucket;
    c.store.accessKey = access;
    c.store.secretKey = secret;
    c.store.sessionToken = envValue ("S3_SESSION_TOKEN");

    if (const auto region = envValue ("S3_REGION"); ! region.empty())
        c.store.region = region;

    if (const char* prefix = std::getenv ("S3_PREFIX"))
        c.prefix = StorageKeys::normalisePrefix (prefix);

    if (const auto style = lowerCase (envValue ("S3_PATH_STYLE")); style == "false" || style == "0" || style == "no")
        c.store.pathStyle = false;

    if (const auto ttl = envValue ("ROVOX_LEASE_TTL_S"); ! ttl.empty())
        c.leaseTtlS = std::max (5, std::atoi (ttl.c_str()));

    return c;
}

json StorageConfig::describe() const
{
    if (! s3)
        return { { "backend", "local" } };

    return { { "backend", "s3" }, { "endpoint", store.endpoint }, { "bucket", store.bucket }, { "region", store.region }, { "prefix", prefix } };
}

std::string StorageKeys::normalisePrefix (std::string p)
{
    while (! p.empty() && p.front() == '/')
        p.erase (0, 1);

    if (! p.empty() && p.back() != '/')
        p += '/';

    return p;
}

//==============================================================================
json ProjectHead::toJson() const
{
    return { { "project_id", projectId }, { "tenant_id", tenant }, { "name", name }, { "revision", revision }, { "modified", modified }, { "tracks", tracks }, { "duration_s", durationS } };
}

std::optional<ProjectHead> ProjectHead::fromJson (const json& j, const std::string& tenant, const std::string& projectId)
{
    if (! j.is_object() || ! j.contains ("revision") || ! j["revision"].is_number_unsigned())
        return std::nullopt;

    ProjectHead h;
    h.tenant = tenant;
    h.projectId = projectId;
    h.name = j.value ("name", "Untitled");
    h.revision = j["revision"].get<std::uint64_t>();
    h.modified = j.value ("modified", (std::int64_t) 0);
    h.tracks = j.value ("tracks", 0);
    h.durationS = j.value ("duration_s", 0.0);
    return h;
}

//==============================================================================
std::int64_t ProjectLease::systemNow()
{
    return (std::int64_t) std::time (nullptr);
}

Result<std::optional<LeaseInfo>> ProjectLease::read (ObjectStore& store, const std::string& key)
{
    auto body = store.get (key);

    if (! body.ok())
    {
        if (body.error().code == errc::notFound)
            return std::optional<LeaseInfo> {};

        return body.error();
    }

    const auto j = json::parse (body.value(), nullptr, false);

    if (j.is_discarded() || ! j.is_object() || ! j.value ("owner", json()).is_string())
        return std::optional<LeaseInfo> {};          // unreadable = nobody's

    return std::optional<LeaseInfo> { LeaseInfo { j["owner"].get<std::string>(), j.value ("expires", (std::int64_t) 0), j.value ("acquired", (std::int64_t) 0) } };
}

namespace
{
Error leaseBusy (const LeaseInfo& other, std::int64_t now)
{
    return makeError (errc::busy, "the project is open for writing by another Rovox server (" + other.owner + "); its lease ends in " + std::to_string (std::max<std::int64_t> (0, other.expires - now))
                                      + " s unless it renews it. Close the project there, or wait for the lease to expire",
                      json { { "lease_owner", other.owner }, { "lease_expires", isoTime (other.expires) } });
}

Result<void> writeLease (ObjectStore& store, const std::string& key, const LeaseInfo& l)
{
    return store.put (key, json { { "owner", l.owner }, { "expires", l.expires }, { "acquired", l.acquired }, { "expires_at", isoTime (l.expires) } }.dump(), "application/json");
}
} // namespace

Result<LeaseInfo> ProjectLease::acquire (ObjectStore& store, const std::string& key, const std::string& owner, int ttlS, const Clock& clock)
{
    auto current = read (store, key);

    if (! current.ok())
        return current.error();

    const auto now = clock();

    if (current.value() && current.value()->owner != owner && current.value()->expires > now)
        return leaseBusy (*current.value(), now);

    LeaseInfo mine { owner, now + ttlS, current.value() && current.value()->owner == owner ? current.value()->acquired : now };

    if (auto w = writeLease (store, key, mine); ! w.ok())
        return w.error();

    // two servers can both pass the check above; the one whose write came last is the one that reads itself back
    auto verify = read (store, key);

    if (! verify.ok())
        return verify.error();

    if (! verify.value() || verify.value()->owner != owner)
        return leaseBusy (verify.value().value_or (LeaseInfo { "another server", now + ttlS, now }), now);

    return mine;
}

Result<LeaseInfo> ProjectLease::renew (ObjectStore& store, const std::string& key, const std::string& owner, int ttlS, const Clock& clock)
{
    auto current = read (store, key);

    if (! current.ok())
        return current.error();

    const auto now = clock();

    if (current.value() && current.value()->owner != owner && current.value()->expires > now)
        return leaseBusy (*current.value(), now);

    LeaseInfo mine { owner, now + ttlS, current.value() && current.value()->owner == owner ? current.value()->acquired : now };

    if (auto w = writeLease (store, key, mine); ! w.ok())
        return w.error();

    return mine;
}

void ProjectLease::release (ObjectStore& store, const std::string& key, const std::string& owner)
{
    auto current = read (store, key);

    if (current.ok() && current.value() && current.value()->owner == owner)
        store.remove (key);
}

//==============================================================================
Result<SnapshotJob> readSnapshot (const fs::path& dir, const fs::path& projectDir, ProjectHead head)
{
    SnapshotJob job;
    job.editXml = readWhole (dir / "edit.xml");
    job.manifestJson = readWhole (dir / "rovox.json");

    if (job.editXml.empty() || job.manifestJson.empty())
        return makeError (errc::notFound, "no edit.xml / rovox.json in " + dir.string());

    const auto m = json::parse (job.manifestJson, nullptr, false);

    if (m.is_discarded() || ! m.is_object())
        return makeError (errc::invalidParams, "rovox.json is not valid JSON: " + dir.string());

    job.projectId = m.value ("project_id", "");
    job.tenant = m.value ("tenant_id", "local");

    if (! isValidId (job.projectId, "p") || ! isValidTenantId (job.tenant))
        return makeError (errc::invalidParams, "rovox.json has no valid project_id / tenant_id: " + dir.string());

    job.revision = m.value ("revision", (std::uint64_t) 0);
    job.projectDir = projectDir;
    head.projectId = job.projectId;
    head.tenant = job.tenant;
    head.revision = job.revision;

    if (head.name.empty())
        head.name = m.value ("name", "Untitled");

    job.head = head;

    for (auto& e : m.value ("media", json::array()))
        if (e.is_object() && ! e.value ("external", false))
            if (const auto sha = e.value ("sha256", ""); mediaExtension (sha, e.value ("path", "")))
                job.media.push_back ({ sha, e.value ("path", "") });

    return job;
}

//==============================================================================
RemoteStorage::RemoteStorage (std::shared_ptr<ObjectStore> store, StorageKeys keys, std::string description, std::string owner, int leaseTtlS)
    : store_ (std::move (store)), keys_ (std::move (keys)), description_ (std::move (description)), owner_ (std::move (owner)), leaseTtlS_ (leaseTtlS)
{}

RemoteStorage::~RemoteStorage()
{
    shutdown (std::chrono::seconds (0));
}

void RemoteStorage::start()
{
    std::lock_guard<std::mutex> lock (mutex_);

    if (started_)
        return;

    started_ = true;
    stop_ = false;
    thread_ = std::thread ([this] { loop(); });
}

bool RemoteStorage::flush (std::chrono::seconds timeout)
{
    std::unique_lock<std::mutex> lock (mutex_);

    if (! started_)
        return queue_.empty();

    flushing_ = true;
    cv_.notify_all();
    const auto done = idleCv_.wait_for (lock, timeout, [this]
    {
        for (auto& [k, job] : queue_)
            if (! flushTried_.count (k))
                return false;

        if (! inFlight_.empty())
            return false;

        for (auto& [k, st] : states_)
            if ((st.releaseWanted || st.forgetWanted) && ! queue_.count (k))
                return false;

        return true;
    });
    flushing_ = false;
    flushTried_.clear();
    return done && queue_.empty();
}

bool RemoteStorage::shutdown (std::chrono::seconds timeout)
{
    const bool wasStarted = [&] { std::lock_guard<std::mutex> lock (mutex_); return started_; }();
    bool clean = true;

    if (wasStarted)
        clean = flush (timeout);

    {
        std::lock_guard<std::mutex> lock (mutex_);
        stop_ = true;
        started_ = false;
    }

    cv_.notify_all();

    if (thread_.joinable())
        thread_.join();

    return clean;
}

json RemoteStorage::describe() const
{
    return { { "backend", "s3" }, { "store", description_ }, { "owner", owner_ } };
}

void RemoteStorage::setListener (StatusListener l)
{
    std::lock_guard<std::mutex> lock (mutex_);
    listener_ = std::move (l);
}

void RemoteStorage::setIntervals (std::chrono::milliseconds headRefresh, std::chrono::milliseconds leaseRenew)
{
    std::lock_guard<std::mutex> lock (mutex_);
    headRefresh_ = headRefresh;
    leaseRenew_ = leaseRenew;
}

//==============================================================================
json RemoteStorage::statusLocked (const Key& k) const
{
    json j { { "backend", "s3" }, { "synced_revision", nullptr }, { "pending_uploads", 0 }, { "last_error", nullptr } };
    int pending = (int) queue_.count (k) + (int) inFlight_.count (k);
    j["pending_uploads"] = pending;

    if (auto it = states_.find (k); it != states_.end())
    {
        if (it->second.synced)
            j["synced_revision"] = *it->second.synced;

        if (! it->second.lastError.empty())
            j["last_error"] = it->second.lastError;

        j["lease"] = it->second.leaseLost ? "lost" : (it->second.leaseHeld ? "held" : "none");
    }

    return j;
}

json RemoteStorage::status (const std::string& tenant, const std::string& projectId) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return statusLocked ({ tenant, projectId });
}

int RemoteStorage::pendingUploads() const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return (int) (queue_.size() + inFlight_.size());
}

void RemoteStorage::emit (const Key& k)
{
    StatusListener l;
    json st;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        l = listener_;
        st = statusLocked (k);
    }

    if (l)
        l (k.first, k.second, st);
}

//==============================================================================
Result<void> RemoteStorage::acquireLease (const std::string& tenant, const std::string& projectId)
{
    const Key k { tenant, projectId };
    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto& st = states_[k];

        if (st.leaseHeld && ! st.leaseLost)
        {
            st.releaseWanted = false;           // closed and opened again before the release went out
            return {};
        }
    }

    auto r = ProjectLease::acquire (*store_, keys_.lease (tenant, projectId), owner_, leaseTtlS_, clock_);

    if (! r.ok())
        return r.error();

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto& st = states_[k];
        st.leaseHeld = true;
        st.leaseLost = false;
        st.releaseWanted = false;
        st.leaseRenewedAt = clock_();
    }

    emit (k);
    return {};
}

void RemoteStorage::releaseLease (const std::string& tenant, const std::string& projectId)
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto it = states_.find ({ tenant, projectId });

        if (it == states_.end() || ! it->second.leaseHeld)
            return;

        it->second.releaseWanted = true;
    }

    cv_.notify_all();
}

void RemoteStorage::forget (const std::string& tenant, const std::string& projectId)
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        states_[{ tenant, projectId }].forgetWanted = true;
    }

    cv_.notify_all();
}

void RemoteStorage::enqueue (SnapshotJob job)
{
    const Key k { job.tenant, job.projectId };
    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto& st = states_[k];
        st.attempts = 0;
        st.notBefore = {};
        queue_[k] = std::move (job);
    }

    emit (k);
    cv_.notify_all();
}

//==============================================================================
void RemoteStorage::setHead (const ProjectHead& h)
{
    std::lock_guard<std::mutex> lock (mutex_);
    heads_[h.tenant][h.projectId] = h;
}

Result<void> RemoteStorage::uploadMedia (const SnapshotJob& job, std::string& warning)
{
    for (auto& m : job.media)
    {
        const auto ext = mediaExtension (m.sha256, m.relPath);

        if (! ext)
            continue;

        const auto key = keys_.media (job.tenant, m.sha256, *ext);
        {
            std::lock_guard<std::mutex> lock (mutex_);

            if (knownMedia_.count (key))
                continue;
        }

        auto present = store_->head (key);

        if (! present.ok() && present.error().code != errc::notFound)
            return present.error();

        if (! present.ok())
        {
            const auto local = job.projectDir / m.relPath;
            std::error_code ec;

            if (! fs::is_regular_file (local, ec))
            {
                warning = "media " + fs::path (m.relPath).filename().string() + " is neither in the cache nor in the bucket";
                continue;
            }

            if (auto up = store_->putFile (key, local, contentTypeFor (local)); ! up.ok())
                return up.error();
        }

        std::lock_guard<std::mutex> lock (mutex_);
        knownMedia_.insert (key);
    }

    return {};
}

Result<void> RemoteStorage::push (const SnapshotJob& job)
{
    if (! isValidId (job.projectId, "p") || ! isValidTenantId (job.tenant))
        return makeError (errc::invalidParams, "invalid project or tenant id");

    // a newer HEAD in the bucket (somebody else wrote it, or our cache was stale) is never overwritten
    auto current = fetchHead (job.tenant, job.projectId);

    if (! current.ok() && current.error().code != errc::notFound)
        return current.error();

    if (current.ok() && current.value().revision > job.revision)
        return makeError (errc::revisionConflict, "the bucket already has revision " + std::to_string (current.value().revision) + " of this project (this server has " + std::to_string (job.revision)
                                                       + "); open the project again to load it",
                          json { { "remote_revision", current.value().revision }, { "local_revision", job.revision } });

    if (current.ok() && current.value().revision == job.revision)
    {
        std::lock_guard<std::mutex> lock (mutex_);              // already there
        auto& st = states_[{ job.tenant, job.projectId }];
        st.synced = job.revision;
        st.lastError.clear();
        return {};
    }

    std::string warning;

    if (auto r = uploadMedia (job, warning); ! r.ok())          // media first: HEAD must never point at a revision whose media is not there
        return r;

    if (auto r = store_->put (keys_.revFile (job.tenant, job.projectId, job.revision, "edit.xml"), job.editXml, "application/xml"); ! r.ok())
        return r;

    if (auto r = store_->put (keys_.revFile (job.tenant, job.projectId, job.revision, "rovox.json"), job.manifestJson, "application/json"); ! r.ok())
        return r;

    auto head = job.head;
    head.revision = job.revision;
    head.modified = clock_();

    if (auto r = store_->put (keys_.head (job.tenant, job.projectId), head.toJson().dump(), "application/json"); ! r.ok())
        return r;

    setHead (head);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto& st = states_[{ job.tenant, job.projectId }];
        st.synced = job.revision;
        st.lastError = warning;
    }
    return {};
}

bool RemoteStorage::processOne (bool ignoreBackoff)
{
    SnapshotJob job;
    Key k;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto it = queue_.begin();

        for (; it != queue_.end(); ++it)
        {
            if (inFlight_.count (it->first))
                continue;

            if (flushing_ && flushTried_.count (it->first))
                continue;

            if (! ignoreBackoff && states_[it->first].notBefore > now)
                continue;

            break;
        }

        if (it == queue_.end())
            return false;

        k = it->first;
        job = std::move (it->second);
        queue_.erase (it);
        inFlight_.insert (k);

        if (flushing_)
            flushTried_.insert (k);
    }

    Result<void> r = makeError (errc::busy, "the lease on this project is lost");
    bool leaseLost = false;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        leaseLost = states_[k].leaseLost;
    }

    if (! leaseLost)
        r = push (job);

    {
        std::lock_guard<std::mutex> lock (mutex_);
        inFlight_.erase (k);
        auto& st = states_[k];

        if (r.ok())
        {
            st.attempts = 0;
        }
        else
        {
            st.lastError = r.error().message;
            ++st.attempts;
            st.notBefore = std::chrono::steady_clock::now() + std::min (std::chrono::milliseconds (60000), retryBase_ * (1 << std::min (st.attempts, 5)));

            if (r.error().code != errc::revisionConflict && ! queue_.count (k))
                queue_[k] = std::move (job);            // retry later with the same snapshot, unless a newer one is queued
        }
    }

    emit (k);
    idleCv_.notify_all();
    return true;
}

void RemoteStorage::housekeeping()
{
    const auto now = std::chrono::steady_clock::now();

    // releases and deletions once the project's queue is empty
    std::vector<std::pair<Key, std::pair<bool, bool>>> due;
    {
        std::lock_guard<std::mutex> lock (mutex_);

        for (auto& [k, st] : states_)
            if ((st.releaseWanted || st.forgetWanted) && ! queue_.count (k) && ! inFlight_.count (k))
                due.push_back ({ k, { st.releaseWanted, st.forgetWanted } });
    }

    for (auto& [k, what] : due)
    {
        if (what.second)
        {
            store_->remove (keys_.head (k.first, k.second));
            store_->remove (keys_.lease (k.first, k.second));
        }
        else if (what.first)
        {
            ProjectLease::release (*store_, keys_.lease (k.first, k.second), owner_);
        }

        std::lock_guard<std::mutex> lock (mutex_);

        if (what.second)
        {
            states_.erase (k);

            if (auto t = heads_.find (k.first); t != heads_.end())
                t->second.erase (k.second);
        }
        else
        {
            auto& st = states_[k];
            st.leaseHeld = false;
            st.releaseWanted = false;
        }
    }

    if (! due.empty())
        idleCv_.notify_all();

    // lease renewal
    const auto renewEvery = leaseRenew_.count() > 0 ? leaseRenew_ : std::chrono::milliseconds (leaseTtlS_ * 1000 / 3);

    if (now - lastRenew_ >= renewEvery)
    {
        lastRenew_ = now;
        std::vector<Key> held;
        {
            std::lock_guard<std::mutex> lock (mutex_);

            for (auto& [k, st] : states_)
                if (st.leaseHeld && ! st.leaseLost && ! st.releaseWanted)
                    held.push_back (k);
        }

        for (auto& k : held)
        {
            auto r = ProjectLease::renew (*store_, keys_.lease (k.first, k.second), owner_, leaseTtlS_, clock_);
            {
                std::lock_guard<std::mutex> lock (mutex_);
                auto& st = states_[k];

                if (r.ok())
                {
                    st.leaseRenewedAt = clock_();
                }
                else if (r.error().code == errc::busy && r.error().details.contains ("lease_owner"))
                {
                    st.leaseLost = true;
                    st.lastError = "the writer lease was taken over: " + r.error().message;
                }
                else
                {
                    st.lastError = "cannot renew the writer lease: " + r.error().message;
                }
            }
            emit (k);
        }
    }

    if (now - lastRefresh_ >= headRefresh_)
    {
        lastRefresh_ = now;
        refreshHeads();
    }
}

void RemoteStorage::loop()
{
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock (mutex_);

            if (stop_)
                return;

            cv_.wait_for (lock, std::chrono::milliseconds (500));

            if (stop_)
                return;
        }

        const bool flushing = [this] { std::lock_guard<std::mutex> lock (mutex_); return flushing_; }();

        while (processOne (flushing))
            ;

        housekeeping();
        idleCv_.notify_all();
    }
}

//==============================================================================
Result<ProjectHead> RemoteStorage::fetchHead (const std::string& tenant, const std::string& projectId)
{
    auto body = store_->get (keys_.head (tenant, projectId));

    if (! body.ok())
        return body.error();

    const auto j = json::parse (body.value(), nullptr, false);

    if (auto h = ProjectHead::fromJson (j, tenant, projectId))
    {
        setHead (*h);
        return *h;
    }

    return makeError (errc::io, "the HEAD object of project " + projectId + " is not valid");
}

Result<std::uint64_t> RemoteStorage::pull (const std::string& tenant, const std::string& projectId, const fs::path& projectDir)
{
    if (! isValidId (projectId, "p") || ! isValidTenantId (tenant))
        return makeError (errc::invalidParams, "invalid project or tenant id");

    auto head = fetchHead (tenant, projectId);

    if (! head.ok())
        return head.error();

    const auto revision = head.value().revision;
    auto edit = store_->get (keys_.revFile (tenant, projectId, revision, "edit.xml"));

    if (! edit.ok())
        return edit.error();

    auto manifest = store_->get (keys_.revFile (tenant, projectId, revision, "rovox.json"));

    if (! manifest.ok())
        return manifest.error();

    const auto m = json::parse (manifest.value(), nullptr, false);

    if (m.is_discarded() || ! m.is_object() || m.value ("project_id", "") != projectId || m.value ("tenant_id", "local") != tenant)
        return makeError (errc::io, "revision " + std::to_string (revision) + " in the bucket does not belong to project " + projectId);

    std::error_code ec;
    fs::create_directories (projectDir / "media", ec);
    std::string missingMedia;

    for (auto& e : m.value ("media", json::array()))
    {
        if (! e.is_object() || e.value ("external", false))
            continue;

        const auto sha = e.value ("sha256", ""), rel = e.value ("path", "");
        const auto ext = mediaExtension (sha, rel);

        if (! ext)
            continue;

        const auto dest = projectDir / rel;

        if (fs::is_regular_file (dest, ec))
            continue;

        if (auto r = store_->getFile (keys_.media (tenant, sha, *ext), dest); ! r.ok())
        {
            if (r.error().code != errc::notFound)
                return r.error();

            missingMedia += (missingMedia.empty() ? "" : ", ") + fs::path (rel).filename().string();
        }
    }

    if (auto r = writeAtomic (projectDir / "edit.xml", edit.value()); ! r.ok())
        return r.error();

    if (auto r = writeAtomic (projectDir / "rovox.json", manifest.value()); ! r.ok())          // the manifest pins the revision: last
        return r.error();

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto& st = states_[{ tenant, projectId }];
        st.synced = revision;
        st.lastError = missingMedia.empty() ? "" : "media missing in the bucket: " + missingMedia;
    }

    emit ({ tenant, projectId });
    return revision;
}

Result<void> RemoteStorage::refreshHeads()
{
    auto tenants = store_->list (keys_.tenantsPrefix(), "/");

    if (! tenants.ok())
        return tenants.error();

    std::map<std::string, std::map<std::string, ProjectHead>> fresh;

    for (auto& tp : tenants.value().prefixes)
    {
        auto name = tp.substr (keys_.tenantsPrefix().size());

        while (! name.empty() && name.back() == '/')
            name.pop_back();

        if (! isValidTenantId (name))
            continue;

        auto projects = store_->list (keys_.projectsPrefix (name), "/");

        if (! projects.ok())
            return projects.error();

        auto& byId = fresh[name];

        for (auto& pp : projects.value().prefixes)
        {
            auto id = pp.substr (keys_.projectsPrefix (name).size());

            while (! id.empty() && id.back() == '/')
                id.pop_back();

            if (! isValidId (id, "p"))
                continue;

            auto body = store_->get (keys_.head (name, id));

            if (! body.ok())
                continue;

            if (auto h = ProjectHead::fromJson (json::parse (body.value(), nullptr, false), name, id))
                byId[id] = *h;
        }
    }

    std::lock_guard<std::mutex> lock (mutex_);

    for (auto& [tenant, byId] : heads_)
        for (auto& [id, h] : byId)
        {
            auto st = states_.find ({ tenant, id });
            const bool ours = st != states_.end() && st->second.synced && *st->second.synced == h.revision && ! st->second.forgetWanted;
            auto& slot = fresh[tenant];

            if (ours && (! slot.count (id) || slot[id].revision < h.revision))
                slot[id] = h;
        }

    heads_ = std::move (fresh);
    return {};
}

std::vector<ProjectHead> RemoteStorage::heads (const std::string& tenant) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    std::vector<ProjectHead> out;

    if (auto it = heads_.find (tenant); it != heads_.end())
        for (auto& [id, h] : it->second)
            out.push_back (h);

    return out;
}

std::optional<ProjectHead> RemoteStorage::cachedHead (const std::string& tenant, const std::string& projectId) const
{
    std::lock_guard<std::mutex> lock (mutex_);

    if (auto t = heads_.find (tenant); t != heads_.end())
        if (auto h = t->second.find (projectId); h != t->second.end())
            return h->second;

    return std::nullopt;
}

std::optional<ProjectHead> RemoteStorage::findByName (const std::string& tenant, const std::string& name) const
{
    std::optional<ProjectHead> best;

    for (auto& h : heads (tenant))
        if (h.name == name && (! best || h.modified >= best->modified))
            best = h;

    return best;
}

//==============================================================================
Result<std::string> RemoteStorage::uploadRender (const std::string& tenant, const std::string& projectId, const fs::path& file)
{
    const auto name = file.filename().string();

    if (! safeName (name))
        return makeError (errc::invalidParams, "invalid render file name");

    const auto key = keys_.render (tenant, projectId, name);

    if (auto r = store_->putFile (key, file, contentTypeFor (file)); ! r.ok())
        return r.error();

    auto sidecar = file;
    sidecar.replace_extension (".render.json");
    std::error_code ec;

    if (fs::is_regular_file (sidecar, ec))
        store_->putFile (keys_.render (tenant, projectId, sidecar.filename().string()), sidecar, "application/json");

    return key;
}

Result<int> RemoteStorage::pushRenders (const std::string& tenant, const std::string& projectId, const fs::path& projectDir)
{
    int uploaded = 0;
    std::error_code ec;

    for (fs::directory_iterator it (projectDir / "renders", ec), end; ! ec && it != end; it.increment (ec))
    {
        const auto name = it->path().filename().string();
        const auto ext = lowerCase (it->path().extension().string());

        if (! it->is_regular_file() || ! safeName (name) || ext == ".part" || ext == ".tmp")
            continue;

        const auto key = keys_.render (tenant, projectId, name);
        auto present = store_->head (key);

        if (present.ok() && present.value().size == (std::uint64_t) fs::file_size (it->path()))
            continue;

        if (present.ok() == false && present.error().code != errc::notFound)
            return present.error();

        if (auto up = store_->putFile (key, it->path(), contentTypeFor (it->path())); ! up.ok())
            return up.error();

        ++uploaded;
    }

    return uploaded;
}

Result<void> RemoteStorage::fetchRender (const std::string& tenant, const std::string& projectId, const std::string& name, const fs::path& dest)
{
    if (! safeName (name))
        return makeError (errc::invalidParams, "invalid render file name");

    return store_->getFile (keys_.render (tenant, projectId, name), dest);
}

std::vector<std::string> RemoteStorage::listRenders (const std::string& tenant, const std::string& projectId)
{
    std::vector<std::string> out;
    const auto prefix = keys_.render (tenant, projectId, "");

    if (auto l = store_->list (prefix); l.ok())
        for (auto& o : l.value().objects)
            out.push_back (o.key.substr (prefix.size()));

    return out;
}

} // namespace rovox
