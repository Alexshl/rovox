// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "ActivityLog.h"
#include "ChangeCollector.h"
#include "EngineHost.h"
#include "Origin.h"
#include "ProjectFormat.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>

namespace rovox
{

class ProjectTransport;
class ProjectMeters;

/** gain / mute / solo of a project in the shared mix (task 100). M1 has no mixed output yet; the state is kept for the
    browser-stream device and for `playing_in_background`. */
struct ProjectMixerStrip
{
    std::atomic<double> gainDb { 0.0 };
    std::atomic<bool> muted { false }, solo { false }, playInBackground { false };
};

struct HistoryStep
{
    std::uint64_t revision = 0;
    std::string label, time;
    Origin origin;

    json toJson() const { return { { "revision", revision }, { "label", label }, { "time", time }, { "origin", origin.toJson() } }; }
};

/** One open project: a Tracktion Edit plus manifest, revision counter and history. The Edit and everything derived
    from it may only be touched on the message thread; revision(), dirty(), id() are safe from anywhere. */
class Project
{
public:
    struct PublishedEvent
    {
        std::uint64_t revision = 0;
        Origin origin;
        std::string undoLabel;
        std::vector<ChangedEntity> changes;
    };

    using PublishFn = std::function<void (Project&, const PublishedEvent&)>;

    Project (EngineHost&, fs::path dir, Manifest, juce::ValueTree editState, PublishFn);
    ~Project();

    const std::string& id() const               { return manifest_.projectId; }
    const std::string& tenant() const           { return manifest_.tenantId; }
    std::string name() const;                   // the (undoable) `rovox:name` of the Edit, else the manifest name
    const fs::path& dir() const                 { return dir_; }
    fs::path mediaDir() const                   { return dir_ / "media"; }
    fs::path rendersDir() const                 { return dir_ / "renders"; }
    EngineHost& host()                          { return host_; }

    te::Edit& edit()                            { return *edit_; }                   // message thread only
    Manifest& manifest()                        { return manifest_; }                // message thread only
    const Manifest& manifest() const            { return manifest_; }
    ActivityLog& activity()                     { return activity_; }
    ProjectMixerStrip& strip()                  { return strip_; }

    std::uint64_t revision() const noexcept     { return revision_.load(); }
    bool dirty() const noexcept                 { return dirty_.load(); }
    std::atomic<int>& activeJobs()              { return activeJobs_; }              // the TTL sweeper never deletes while > 0
    std::vector<HistoryStep>& undoSteps()       { return undoSteps_; }
    std::vector<HistoryStep>& redoSteps()       { return redoSteps_; }
    std::map<std::string, json>& importPlans()  { return importPlans_; }

    /** Transport and meters are created on first use (message thread). */
    ProjectTransport& transport();
    ProjectMeters& meters();

    /** Highest revision a person accepted for the agent highlight (`agent.accept`). Not part of the undo history. */
    std::atomic<std::uint64_t>& acceptedRevision()      { return acceptedRevision_; }

    /** Publishes a non-revision event of this project (transport.changed ...); set by the ProjectManager. */
    std::function<void (const json&)> onEvent;

    /** Called on the message thread after a save (dir() holds the new files) or an autosave (autosave/<revision>.rvx does); set by the ProjectManager
        when an object store mirrors the project. */
    std::function<void (const fs::path& writtenDir, bool autosave)> onPersisted;
    /** `storage` block of project.get / project.list: {backend, synced_revision, pending_uploads, last_error}. */
    std::function<json()> storageStatus;

    /** Saves edit.xml + rovox.json (revision pinned). Clears dirty. */
    Result<void> save (const SaveOptions& = {});
    Result<void> autosave();

    // ---- lookups by stable id (never by index) ----
    te::Track* findTrack (const std::string& id);               // "master" is not a Track here; see isMaster()
    static bool isMaster (const std::string& id)               { return id == "master"; }
    te::Clip* findClip (const std::string& id);
    te::Plugin* findPlugin (const std::string& id);
    static std::string idOf (const juce::ValueTree& state);

    // ---- used by the CommandBus ----
    ChangeCollector& collector()                { return *collector_; }
    /** One revision bump + one published event. Marks dirty. */
    std::uint64_t commit (const Origin&, const std::string& undoLabel, std::vector<ChangedEntity>);
    void markClean()                            { dirty_ = false; }

private:
    EngineHost& host_;
    fs::path dir_;
    Manifest manifest_;
    PublishFn publish_;
    std::unique_ptr<te::Edit> edit_;
    std::unique_ptr<ChangeCollector> collector_;
    ActivityLog activity_;
    ProjectMixerStrip strip_;
    std::atomic<std::uint64_t> revision_ { 0 };
    std::atomic<bool> dirty_ { false };
    std::atomic<int> activeJobs_ { 0 };
    std::atomic<std::uint64_t> acceptedRevision_ { 0 };
    std::unique_ptr<ProjectTransport> transport_;
    std::unique_ptr<ProjectMeters> meters_;
    std::vector<HistoryStep> undoSteps_, redoSteps_;
    std::map<std::string, json> importPlans_;
    int lockFd_ = -1;

    friend class ProjectManager;
};

/** Creates an Edit from a ValueTree whose relative media paths resolve against `dir` (the .rvx directory).
    A newly created master fader is at 0 dB (Tracktion's default is -3 dB), so renders are transparent. */
std::unique_ptr<te::Edit> createEditForProject (te::Engine&, const fs::path& dir, juce::ValueTree state, te::Edit::EditRole role);

} // namespace rovox
