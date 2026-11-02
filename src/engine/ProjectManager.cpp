// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ProjectManager.h"

#include "Ids.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <set>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rovox
{

json ProjectSummary::toJson() const
{
    json j { { "project_id", id }, { "name", name }, { "tenant_id", tenant }, { "revision", revision }, { "dirty", dirty },
             { "tracks", tracks }, { "path", dir.string() }, { "ephemeral", ephemeral } };
    j["ttl_s"] = ttlS.has_value() ? json (*ttlS) : json (nullptr);
    return j;
}

ProjectManager::ProjectManager (EngineHost& host, EventHub& hub) : host_ (host), hub_ (hub) {}

ProjectManager::~ProjectManager()
{
    closeAll (SaveMode::discard);

    if (storage_)
        storage_->setListener ({});         // the storage may outlive us; its uploader thread must not publish into a dead hub
}

fs::path ProjectManager::projectsRoot (const std::string& tenant) const
{
    return tenant == "local" ? host_.projectsDir() : host_.dataDir() / "tenants" / tenant / "projects";
}

fs::path ProjectManager::projectDir (const std::string& tenant, const std::string& id) const
{
    return projectsRoot (tenant) / id / "project.rvx";
}

Result<int> ProjectManager::acquireLock (const fs::path& dir)
{
    const auto lockFile = dir / "project.lock";
    const int fd = ::open (lockFile.c_str(), O_CREAT | O_RDWR, 0644);

    if (fd < 0)
        return makeError (errc::io, "cannot create " + lockFile.string());

    if (::flock (fd, LOCK_EX | LOCK_NB) != 0)
    {
        char buf[32] = {};
        const auto n = ::read (fd, buf, sizeof (buf) - 1);
        ::close (fd);
        return makeError (errc::busy, "project is open in another Rovox process (pid " + std::string (n > 0 ? buf : "?")
                                          + "): " + dir.string() + "; stop that process or open the project read-only");
    }

    const auto pid = std::to_string ((long) ::getpid());
    [[maybe_unused]] auto r1 = ::ftruncate (fd, 0);
    [[maybe_unused]] auto r2 = ::write (fd, pid.c_str(), pid.size());
    return fd;
}

Result<std::string> ProjectManager::attach (const fs::path& dir, LoadedProject loaded, const OpenOptions& opts, bool created)
{
    int lockFd = -1;
    const bool mirrored = storage_ != nullptr && ! opts.readOnly && ! loaded.manifest.ephemeral;

    if (! opts.readOnly)
    {
        auto lock = acquireLock (dir);

        if (! lock.ok())
            return lock.error();

        lockFd = lock.value();
    }

    const auto id = loaded.manifest.projectId;

    if (mirrored)
        if (auto lease = storage_->acquireLease (loaded.manifest.tenantId, id); ! lease.ok() && lease.error().details.contains ("lease_owner"))
        {
            if (lockFd >= 0)
                ::close (lockFd);

            return lease.error();
        }

    if (loaded.manifest.name.empty())
        loaded.manifest.name = "Untitled";

    auto project = std::make_unique<Project> (host_, dir, std::move (loaded.manifest), std::move (loaded.editState),
                                              [this] (Project& p, const Project::PublishedEvent& e) { publish (p, e); });
    project->lockFd_ = lockFd;
    project->onEvent = [this, raw = project.get()] (const json& e) { hub_.publish (raw->tenant(), raw->id(), e); };
    project->storageStatus = [this, tenant = project->tenant(), id] { return storageStatus (tenant, id); };

    if (mirrored)
        project->onPersisted = [this, raw = project.get()] (const fs::path& written, bool) { queueSnapshot (*raw, written); };

    if (created)
    {
        if (auto r = project->save(); ! r.ok())
            return r.error();
    }

    auto* raw = project.get();
    projects_[id] = std::move (project);
    refreshSnapshot (*raw);

    // a project that is newer here than in the bucket (never pushed, or the bucket was empty) goes up now
    if (mirrored && ! created)
        if (auto head = storage_->cachedHead (raw->tenant(), id); ! head || head->revision < raw->revision())
            queueSnapshot (*raw, dir);

    hub_.publish (raw->tenant(), "", { { "type", "projects.changed" }, { "project_id", id }, { "change", "opened" } });
    return id;
}

Result<std::string> ProjectManager::create (const CreateOptions& o)
{
    if (! isValidTenantId (o.tenant))
        return makeError (errc::invalidParams, "invalid tenant_id");

    if (o.ttlS.has_value() && *o.ttlS < 1)
        return makeError (errc::invalidParams, "ttl_s must be >= 1");

    LoadedProject lp;
    lp.manifest.projectId = newId ("p");
    lp.manifest.name = o.name;
    lp.manifest.sampleRate = o.sampleRate;
    lp.manifest.tenantId = o.tenant;
    lp.manifest.ephemeral = o.ephemeral;
    lp.manifest.ttlS = o.ttlS;
    lp.manifest.createdAt = nowIso8601();
    lp.editState = te::createEmptyEdit (host_.engine());

    const auto dir = o.dir.empty() ? projectDir (o.tenant, lp.manifest.projectId) : o.dir;
    std::error_code ec;
    fs::create_directories (dir, ec);

    if (fs::exists (dir / manifestFileName))
        return makeError (errc::preconditionFailed, "a project already exists at " + dir.string());

    return attach (dir, std::move (lp), { .tenant = o.tenant }, true);
}

Result<std::string> ProjectManager::open (const std::string& target, const OpenOptions& opts)
{
    if (! isValidTenantId (opts.tenant))
        return makeError (errc::invalidParams, "invalid tenant_id");

    fs::path dir;
    bool explicitPath = false;

    if (isValidId (target, "p"))
    {
        if (auto* p = find (target, opts.tenant))
            return p->id();

        dir = projectDir (opts.tenant, target);
    }
    else if (target.find ('/') != std::string::npos || target.rfind ('.', 0) == 0)
    {
        // anything that looks like a path is checked against the path policy before it is even looked at (no probing outside)
        auto allowed = host_.checkPath (target);

        if (! allowed.ok())
            return allowed.error();

        if (std::error_code probe; ! fs::exists (allowed.value(), probe))
            return makeError (errc::notFound, "no project at " + target);

        dir = allowed.value();
        explicitPath = true;
    }
    else if (std::error_code probe; fs::exists (target, probe))
    {
        auto allowed = host_.checkPath (target);

        if (! allowed.ok())
            return allowed.error();

        dir = allowed.value();
        explicitPath = true;
    }
    else if (auto byName = findDirByName (opts.tenant, target))
    {
        dir = *byName;
    }
    else if (auto remote = findRemoteDirByName (opts.tenant, target))
    {
        dir = *remote;
    }
    else
    {
        return makeError (errc::notFound, "no project '" + target + "' (not a path, a project id or a project name in " + projectsRoot (opts.tenant).string() + ")");
    }

    // a project of the bucket: take the writer lease and bring the cache up to HEAD before anything is read
    std::string leasedId;

    if (storage_ != nullptr && ! explicitPath && dir.filename() == "project.rvx")
    {
        const auto remoteId = dir.parent_path().filename().string();

        if (isValidId (remoteId, "p"))
        {
            if (auto* alreadyOpen = find (remoteId, opts.tenant))
                return alreadyOpen->id();

            if (auto r = syncDown (opts.tenant, remoteId, dir, opts.readOnly); ! r.ok())
                return r.error();

            if (! opts.readOnly)
                leasedId = remoteId;
        }
    }

    auto fail = [&] (Error e) -> Result<std::string>
    {
        if (! leasedId.empty() && find (leasedId) == nullptr)
            storage_->releaseLease (opts.tenant, leasedId);

        return e;
    };

    std::error_code ec;
    const auto canonical = fs::weakly_canonical (dir, ec);

    for (auto& [id, p] : projects_)
        if (fs::weakly_canonical (p->dir(), ec) == canonical)
            return id;

    auto loaded = opts.preferAutosave ? Result<LoadedProject> (makeError (errc::notFound, "no autosave")) : readProject (dir);

    // an autosave newer than the main files means the server stopped with unsaved work (shutdown autosaves, it does not save):
    // recover it instead of silently opening the older state
    if (opts.preferAutosave)
    {
        const auto autosaves = listAutosaves (dir);

        if (! autosaves.empty())
            loaded = readAutosave (dir, autosaves.back());
    }
    else if (loaded.ok() && loaded.value().newerAutosave)
    {
        if (auto a = readAutosave (dir, *loaded.value().newerAutosave); a.ok())
            loaded = std::move (a);
    }

    if (! loaded.ok())
        return fail (loaded.error());

    if (! explicitPath && loaded.value().manifest.tenantId != opts.tenant)
        return fail (makeError (errc::notFound, "project " + target + " does not belong to tenant " + opts.tenant));

    auto attached = attach (dir, std::move (loaded.value()), opts, false);

    if (! attached.ok())
        return fail (attached.error());

    return attached;
}

Result<void> ProjectManager::syncDown (const std::string& tenant, const std::string& id, const fs::path& dir, bool readOnly)
{
    const bool haveLocal = fs::exists (dir / manifestFileName);

    if (! readOnly)
    {
        // another live writer is final; an unreachable bucket is not: the working cache can still be opened (pushes retry in the background)
        if (auto lease = storage_->acquireLease (tenant, id); ! lease.ok() && lease.error().details.contains ("lease_owner"))
            return lease.error();
    }

    auto head = storage_->fetchHead (tenant, id);

    if (! head.ok())
    {
        if (haveLocal)
            return {};                  // not pushed yet, or the bucket cannot be reached

        if (! readOnly)
            storage_->releaseLease (tenant, id);

        return head.error();
    }

    std::uint64_t localRevision = 0;

    if (haveLocal)
    {
        std::ifstream in (dir / manifestFileName);
        const auto doc = json::parse (in, nullptr, false);

        if (doc.is_object())
            localRevision = doc.value ("revision", (std::uint64_t) 0);

        for (auto r : listAutosaves (dir))
            localRevision = std::max (localRevision, r);
    }

    if (haveLocal && localRevision >= head.value().revision)
        return {};

    auto pulled = storage_->pull (tenant, id, dir);

    if (! pulled.ok() && ! haveLocal)
    {
        if (! readOnly)
            storage_->releaseLease (tenant, id);

        return pulled.error();
    }

    return {};
}

std::optional<fs::path> ProjectManager::findRemoteDirByName (const std::string& tenant, const std::string& name)
{
    if (storage_ == nullptr)
        return std::nullopt;

    auto head = storage_->findByName (tenant, name);

    if (! head)
    {
        storage_->refreshHeads();
        head = storage_->findByName (tenant, name);
    }

    if (! head)
        return std::nullopt;

    return projectDir (tenant, head->projectId);
}

void ProjectManager::queueSnapshot (Project& p, const fs::path& writtenDir)
{
    if (storage_ == nullptr)
        return;

    ProjectHead head;
    head.name = p.name();
    head.tracks = (int) te::getAudioTracks (p.edit()).size();
    head.durationS = p.edit().getLength().inSeconds();

    if (auto job = readSnapshot (writtenDir, p.dir(), head); job.ok())
        storage_->enqueue (std::move (job.value()));
}

void ProjectManager::setStorage (std::shared_ptr<RemoteStorage> s)
{
    storage_ = std::move (s);

    if (storage_)
        storage_->setListener ([this] (const std::string& tenant, const std::string& id, const json& status)
                               { hub_.publish (tenant, id, { { "type", "storage.changed" }, { "project_id", id }, { "storage", status } }); });
}

json ProjectManager::storageStatus (const std::string& tenant, const std::string& id) const
{
    if (storage_ == nullptr)
        return { { "backend", "local" }, { "synced_revision", nullptr }, { "pending_uploads", 0 }, { "last_error", nullptr } };

    return storage_->status (tenant, id);
}

std::optional<fs::path> ProjectManager::findDirByName (const std::string& tenant, const std::string& name) const
{
    std::optional<fs::path> best;
    std::string bestCreated;
    std::error_code ec;

    for (auto& entry : fs::directory_iterator (projectsRoot (tenant), ec))
    {
        std::ifstream in (entry.path() / "project.rvx" / manifestFileName);

        if (! in)
            continue;

        std::stringstream ss;
        ss << in.rdbuf();
        const auto doc = json::parse (ss.str(), nullptr, false);

        if (! doc.is_discarded() && doc.value ("name", "") == name && doc.value ("created_at", "") >= bestCreated)
        {
            best = entry.path() / "project.rvx";
            bestCreated = doc.value ("created_at", "");
        }
    }

    return best;
}

Result<void> ProjectManager::close (const std::string& id, SaveMode mode)
{
    auto it = projects_.find (id);

    if (it == projects_.end())
        return makeError (errc::notFound, "no open project " + id);

    if (it->second->activeJobs() > 0)
        return makeError (errc::busy, "a job is running on project " + id);

    Result<void> result;

    if (mode == SaveMode::save)
        result = it->second->save();
    else if (mode == SaveMode::autosave && it->second->dirty())
        result = it->second->autosave();

    const auto tenant = it->second->tenant();
    projects_.erase (it);

    if (storage_)
        storage_->releaseLease (tenant, id);        // after the snapshot that the save above queued

    {
        std::lock_guard<std::mutex> lock (snapshotMutex_);
        snapshot_.erase (id);
    }

    hub_.publish (tenant, "", { { "type", "projects.changed" }, { "project_id", id }, { "change", "closed" } });
    return result;
}

void ProjectManager::closeAll (SaveMode mode)
{
    std::vector<std::string> ids;

    for (auto& [id, p] : projects_)
        ids.push_back (id);

    for (auto& id : ids)
        if (auto* p = find (id); p != nullptr)
        {
            p->activeJobs() = 0;
            close (id, mode);
        }
}

Result<void> ProjectManager::remove (const std::string& id, const std::string& tenant)
{
    auto* p = find (id, tenant);

    if (p == nullptr)
        return makeError (errc::notFound, "no open project " + id);

    if (p->activeJobs() > 0)
        return makeError (errc::busy, "a job is running on project " + id);

    const auto dir = p->dir();
    const bool mirrored = storage_ != nullptr && ! p->manifest().ephemeral;
    close (id, SaveMode::discard);

    if (mirrored)
        storage_->forget (tenant, id);

    std::error_code ec;
    const bool managed = dir.filename() == "project.rvx" && dir.parent_path().filename() == id;
    fs::remove_all (managed ? dir.parent_path() : dir, ec);
    return ec ? Result<void> (makeError (errc::io, "cannot delete " + dir.string() + ": " + ec.message())) : Result<void>();
}

Project* ProjectManager::find (const std::string& id)
{
    auto it = projects_.find (id);
    return it == projects_.end() ? nullptr : it->second.get();
}

Project* ProjectManager::find (const std::string& id, const std::string& tenant)
{
    auto* p = find (id);
    return (p != nullptr && p->tenant() == tenant) ? p : nullptr;
}

std::vector<ProjectSummary> ProjectManager::list (const std::string& tenant, bool includeEphemeral) const
{
    std::lock_guard<std::mutex> lock (snapshotMutex_);
    std::vector<ProjectSummary> out;

    for (auto& [id, s] : snapshot_)
        if (s.tenant == tenant && (includeEphemeral || ! s.ephemeral))
            out.push_back (s);

    return out;
}

json ProjectManager::catalog (const std::string& tenant, bool includeEphemeral) const
{
    std::map<std::string, ProjectSummary> open;
    {
        std::lock_guard<std::mutex> lock (snapshotMutex_);

        for (auto& [id, s] : snapshot_)
            if (s.tenant == tenant)
                open[id] = s;
    }

    std::vector<json> items;
    std::set<std::string> seen;
    std::error_code ec;

    auto epoch = [] (const fs::path& f)
    {
        struct stat st {};
        return ::stat (f.c_str(), &st) == 0 ? (long long) st.st_mtime : 0LL;
    };

    for (auto& entry : fs::directory_iterator (projectsRoot (tenant), ec))
    {
        const auto dir = entry.path() / "project.rvx";
        std::ifstream in (dir / manifestFileName);

        if (! in)
            continue;

        auto doc = json::parse (in, nullptr, false);

        if (doc.is_discarded() || ! doc.is_object() || doc.value ("project_id", "").empty() || doc.value ("tenant_id", "local") != tenant)
            continue;

        const auto id = doc["project_id"].get<std::string>();
        const bool ephemeral = doc.value ("ephemeral", false);

        if (ephemeral && ! includeEphemeral && open.count (id) == 0)
            continue;

        json item { { "project_id", id }, { "name", doc.value ("name", "Untitled") }, { "tenant_id", tenant }, { "revision", doc.value ("revision", 0) }, { "dirty", false },
                    { "path", dir.string() }, { "ephemeral", ephemeral }, { "ttl_s", doc.value ("ttl_s", json (nullptr)) }, { "open", false },
                    { "created_at", doc.value ("created_at", "") }, { "saved_at", doc.value ("saved_at", "") },
                    { "modified", epoch (dir / editFileName) }, { "tracks", 0 } };

        if (auto o = open.find (id); o != open.end())
        {
            item["open"] = true;
            item["name"] = o->second.name;
            item["revision"] = o->second.revision;
            item["dirty"] = o->second.dirty;
            item["tracks"] = o->second.tracks;
            item["path"] = o->second.dir.string();
        }
        else if (std::error_code sz; fs::file_size (dir / editFileName, sz) < 64u * 1024u * 1024u && ! sz)
        {
            if (auto xml = juce::parseXML (toFile (dir / editFileName)))
            {
                int tracks = 0;

                for (auto* c : xml->getChildIterator())
                    if (c->hasTagName ("TRACK"))
                        ++tracks;

                item["tracks"] = tracks;
            }
        }

        seen.insert (id);
        items.push_back (std::move (item));
    }

    for (auto& [id, s] : open)                      // opened from a path outside the projects directory
        if (seen.count (id) == 0 && (includeEphemeral || ! s.ephemeral))
        {
            auto item = s.toJson();
            item["open"] = true;
            item["modified"] = (long long) std::time (nullptr);
            items.push_back (std::move (item));
        }

    // the bucket is the source of truth: its projects that this server has no copy of (yet) are listed from their HEAD objects
    if (storage_ != nullptr)
        for (auto& h : storage_->heads (tenant))
        {
            const auto it = std::find_if (items.begin(), items.end(), [&] (const json& i) { return i.value ("project_id", "") == h.projectId; });

            if (it != items.end())
            {
                (*it)["remote_revision"] = h.revision;
                continue;
            }

            items.push_back ({ { "project_id", h.projectId }, { "name", h.name }, { "tenant_id", tenant }, { "revision", h.revision }, { "dirty", false }, { "path", "" },
                               { "ephemeral", false }, { "ttl_s", nullptr }, { "open", false }, { "created_at", "" }, { "saved_at", "" }, { "modified", (long long) h.modified },
                               { "tracks", h.tracks }, { "duration_s", h.durationS }, { "remote_only", true }, { "remote_revision", h.revision } });
        }

    std::sort (items.begin(), items.end(), [] (const json& a, const json& b) { return a.value ("modified", 0LL) > b.value ("modified", 0LL); });
    return items;
}

std::optional<std::uint64_t> ProjectManager::revisionOf (const std::string& id) const
{
    std::lock_guard<std::mutex> lock (snapshotMutex_);
    auto it = snapshot_.find (id);

    if (it == snapshot_.end())
        return std::nullopt;

    return it->second.revision;
}

void ProjectManager::refreshSnapshot (Project& p)
{
    ProjectSummary s;
    s.id = p.id();
    s.name = p.name();
    s.tenant = p.tenant();
    s.dir = p.dir();
    s.revision = p.revision();
    s.dirty = p.dirty();
    s.ephemeral = p.manifest().ephemeral;
    s.ttlS = p.manifest().ttlS;
    s.tracks = (int) te::getAudioTracks (p.edit()).size();

    std::lock_guard<std::mutex> lock (snapshotMutex_);
    snapshot_[s.id] = std::move (s);
}

void ProjectManager::publish (Project& p, const Project::PublishedEvent& e)
{
    json changes = json::array();

    for (auto& c : e.changes)
        changes.push_back (c.toJson());

    hub_.publish (p.tenant(), p.id(), { { "type", "project.changed" }, { "project_id", p.id() }, { "revision", e.revision },
                                         { "origin", e.origin.toJson() }, { "undo_label", e.undoLabel }, { "changes", changes } });
    refreshSnapshot (p);
}

int ProjectManager::autosaveDirty()
{
    int n = 0;

    for (auto& [id, p] : projects_)
        if (p->dirty() && p->autosave().ok())
            ++n;

    return n;
}

namespace
{
std::time_t parseIso (const std::string& s)
{
    std::tm tm {};

    if (strptime (s.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr)
        return 0;

    return timegm (&tm);
}
} // namespace

std::vector<std::string> ProjectManager::sweepExpired (std::time_t now)
{
    std::vector<std::string> removed;
    auto logDeletion = [&] (const std::string& id)
    {
        json entry { { "t", nowIso8601() }, { "origin", "system:ttl" }, { "name", "project.delete" }, { "project_id", id }, { "ok", true } };
        ttlLog_.push_back (entry);
        removed.push_back (id);
    };

    // open projects
    std::vector<std::string> openExpired;

    for (auto& [id, p] : projects_)
    {
        const auto& m = p->manifest();

        if (m.ephemeral && m.ttlS.has_value() && parseIso (m.createdAt) + *m.ttlS < now && p->activeJobs() == 0)
            openExpired.push_back (id);
    }

    for (auto& id : openExpired)
    {
        auto* p = find (id);
        p->activity().add ({ { "t", nowIso8601() }, { "origin", "system:ttl" }, { "name", "project.delete" }, { "ok", true } });

        if (remove (id, p->tenant()).ok())
            logDeletion (id);
    }

    // projects on disk that are not open
    std::vector<fs::path> roots { projectsRoot ("local") };
    std::error_code ec;

    for (auto& t : fs::directory_iterator (host_.dataDir() / "tenants", ec))
        roots.push_back (t.path() / "projects");

    for (auto& root : roots)
        for (auto& entry : fs::directory_iterator (root, ec))
        {
            const auto rvx = entry.path() / "project.rvx";
            std::ifstream in (rvx / manifestFileName);

            if (! in || find (entry.path().filename().string()) != nullptr)
                continue;

            std::stringstream ss;
            ss << in.rdbuf();
            const auto doc = json::parse (ss.str(), nullptr, false);

            if (doc.is_discarded() || ! doc.value ("ephemeral", false) || ! doc.contains ("ttl_s") || ! doc["ttl_s"].is_number_integer())
                continue;

            if (parseIso (doc.value ("created_at", "")) + doc["ttl_s"].get<int>() >= now)
                continue;

            // a process that has the project open holds its flock: leave it alone
            auto lock = acquireLock (rvx);

            if (! lock.ok())
                continue;

            ::close (lock.value());
            fs::remove_all (entry.path(), ec);
            logDeletion (entry.path().filename().string());
        }

    return removed;
}

} // namespace rovox
