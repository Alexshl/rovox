// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ProjectManager.h"

#include "Ids.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
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

    if (! opts.readOnly)
    {
        auto lock = acquireLock (dir);

        if (! lock.ok())
            return lock.error();

        lockFd = lock.value();
    }

    const auto id = loaded.manifest.projectId;

    if (loaded.manifest.name.empty())
        loaded.manifest.name = "Untitled";

    auto project = std::make_unique<Project> (host_, dir, std::move (loaded.manifest), std::move (loaded.editState),
                                              [this] (Project& p, const Project::PublishedEvent& e) { publish (p, e); });
    project->lockFd_ = lockFd;
    project->onEvent = [this, raw = project.get()] (const json& e) { hub_.publish (raw->tenant(), raw->id(), e); };

    if (created)
    {
        if (auto r = project->save(); ! r.ok())
            return r.error();
    }

    auto* raw = project.get();
    projects_[id] = std::move (project);
    refreshSnapshot (*raw);
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
    else
    {
        return makeError (errc::notFound, "no project '" + target + "' (not a path, a project id or a project name in " + projectsRoot (opts.tenant).string() + ")");
    }

    std::error_code ec;
    const auto canonical = fs::weakly_canonical (dir, ec);

    for (auto& [id, p] : projects_)
        if (fs::weakly_canonical (p->dir(), ec) == canonical)
            return id;

    auto loaded = opts.preferAutosave ? Result<LoadedProject> (makeError (errc::notFound, "no autosave")) : readProject (dir);

    if (opts.preferAutosave)
    {
        const auto autosaves = listAutosaves (dir);

        if (! autosaves.empty())
            loaded = readAutosave (dir, autosaves.back());
    }

    if (! loaded.ok())
        return loaded.error();

    if (! explicitPath && loaded.value().manifest.tenantId != opts.tenant)
        return makeError (errc::notFound, "project " + target + " does not belong to tenant " + opts.tenant);

    return attach (dir, std::move (loaded.value()), opts, false);
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
    close (id, SaveMode::discard);

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
