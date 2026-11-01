// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "EventHub.h"
#include "Project.h"

#include <map>
#include <mutex>
#include <optional>

namespace rovox
{

struct CreateOptions
{
    std::string name = "Untitled";
    std::string tenant = "local";
    bool ephemeral = false;
    std::optional<int> ttlS;
    double sampleRate = 44100.0;
    fs::path dir;               // empty = <data>/projects/<id>/project.rvx
};

struct OpenOptions
{
    std::string tenant = "local";   // only used for the project-id form and for validation
    bool readOnly = false;          // no project.lock (used when another process holds it)
    bool preferAutosave = false;    // crash recovery: open the newest autosave instead of the main files
};

enum class SaveMode { save, autosave, discard };

struct ProjectSummary
{
    std::string id, name, tenant;
    fs::path dir;
    std::uint64_t revision = 0;
    bool dirty = false, ephemeral = false;
    std::optional<int> ttlS;
    int tracks = 0;

    json toJson() const;
};

/** Owns the open projects. Message thread only, except list() / revisionOf() which read a mutex-guarded snapshot. */
class ProjectManager
{
public:
    ProjectManager (EngineHost&, EventHub&);
    ~ProjectManager();

    Result<std::string> create (const CreateOptions&);
    /** `target` is a .rvx directory, a project id, or the name of a project in the data dir of `opts.tenant`
        (the newest one if several share it). */
    Result<std::string> open (const std::string& target, const OpenOptions& = {});
    Result<void> close (const std::string& id, SaveMode);
    /** Closes and deletes the project directory. Refused while a job is running on it. */
    Result<void> remove (const std::string& id, const std::string& tenant);
    void closeAll (SaveMode);

    Project* find (const std::string& id);
    Project* find (const std::string& id, const std::string& tenant);      // nullptr for another tenant's project

    std::vector<ProjectSummary> list (const std::string& tenant, bool includeEphemeral = false) const;
    std::optional<std::uint64_t> revisionOf (const std::string& id) const;

    fs::path projectDir (const std::string& tenant, const std::string& id) const;
    /** Project directory of the newest project on disk whose manifest name equals `name`. */
    std::optional<fs::path> findDirByName (const std::string& tenant, const std::string& name) const;
    fs::path projectsRoot (const std::string& tenant) const;

    /** Autosaves every dirty project (called by a 60 s timer in `serve` and on shutdown). */
    int autosaveDirty();
    /** Deletes ephemeral projects past created_at + ttl_s (open or on disk) that have no running job.
        Each deletion is logged with origin "system:ttl"; returns the ids removed. */
    std::vector<std::string> sweepExpired (std::time_t now = std::time (nullptr));
    const std::vector<json>& ttlLog() const     { return ttlLog_; }

    EngineHost& host()      { return host_; }
    EventHub& hub()         { return hub_; }

private:
    Result<std::string> attach (const fs::path& dir, LoadedProject, const OpenOptions&, bool created);
    Result<int> acquireLock (const fs::path& dir);
    void publish (Project&, const Project::PublishedEvent&);
    void refreshSnapshot (Project&);

    EngineHost& host_;
    EventHub& hub_;
    std::map<std::string, std::unique_ptr<Project>> projects_;
    mutable std::mutex snapshotMutex_;
    std::map<std::string, ProjectSummary> snapshot_;
    std::vector<json> ttlLog_;
};

} // namespace rovox
