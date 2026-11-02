// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Project.h"

#include "Transport.h"

#include <unistd.h>

namespace rovox
{

std::unique_ptr<te::Edit> createEditForProject (te::Engine& engine, const fs::path& dir, juce::ValueTree state, te::Edit::EditRole role)
{
    te::Edit::Options o { engine };
    o.editState = std::move (state);
    o.editProjectItemID = te::ProjectItemID::createNewID (te::ProjectID {});
    o.role = role;
    o.numAudioTracks = 0;
    o.defaultMasterVolumedB = 0.0f;

    const auto editFile = toFile (dir / editFileName);
    o.editFileRetriever = [editFile] { return editFile; };
    return te::Edit::createEdit (std::move (o));
}

Project::Project (EngineHost& host, fs::path dir, Manifest manifest, juce::ValueTree editState, PublishFn publish)
    : host_ (host), dir_ (std::move (dir)), manifest_ (std::move (manifest)), publish_ (std::move (publish)), revision_ (manifest_.revision)
{
    std::error_code ec;

    for (const char* sub : { "media", "renders", "peaks", "autosave" })
        fs::create_directories (dir_ / sub, ec);

    edit_ = createEditForProject (host.engine(), dir_, std::move (editState), te::Edit::EditRole::forEditing);

    ChangeCollector::assignMissingIds (edit_->state);
    edit_->getUndoManager().clearUndoHistory();

    collector_ = std::make_unique<ChangeCollector> (edit_->state, [this] (std::vector<ChangedEntity> changes)
    {
        commit (Origin::system(), "system", std::move (changes));
    });

    // let the Edit finish its asynchronous initialisation (master fader and friends are written into the tree a moment later),
    // then start counting revisions from a quiet state
    juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
    collector_->discardPending();
}

Project::~Project()
{
    transport_.reset();
    meters_.reset();

    if (auto* s = host_.stream())
        s->forget (edit_.get());

    collector_.reset();
    edit_.reset();

    if (lockFd_ >= 0)
        ::close (lockFd_);
}

ProjectTransport& Project::transport()
{
    if (transport_ == nullptr)
        transport_ = std::make_unique<ProjectTransport> (*this, [this] (const json& e) { if (onEvent) onEvent (e); });

    return *transport_;
}

ProjectMeters& Project::meters()
{
    if (meters_ == nullptr)
        meters_ = std::make_unique<ProjectMeters> (*this);

    return *meters_;
}

std::string Project::name() const
{
    static const juce::Identifier nameId ("rovox:name");

    if (edit_ != nullptr && edit_->state.hasProperty (nameId))
        return edit_->state[nameId].toString().toStdString();

    return manifest_.name;
}

std::string Project::idOf (const juce::ValueTree& state)
{
    return state[prop::id].toString().toStdString();
}

te::Track* Project::findTrack (const std::string& id)
{
    for (auto* t : te::getAllTracks (*edit_))
        if (idOf (t->state) == id)
            return t;

    return nullptr;
}

te::Clip* Project::findClip (const std::string& id)
{
    for (auto* t : te::getClipTracks (*edit_))
        for (auto* c : t->getClips())
            if (idOf (c->state) == id)
                return c;

    return nullptr;
}

te::Plugin* Project::findPlugin (const std::string& id)
{
    for (auto p : te::getAllPlugins (*edit_, true))
        if (idOf (p->state) == id)
            return p;

    return nullptr;
}

std::uint64_t Project::commit (const Origin& origin, const std::string& undoLabel, std::vector<ChangedEntity> changes)
{
    const auto rev = ++revision_;
    manifest_.revision = rev;
    dirty_ = true;

    if (publish_)
        publish_ (*this, { rev, origin, undoLabel, std::move (changes) });

    return rev;
}

Result<void> Project::save (const SaveOptions& opt)
{
    collector_->beginScope();       // flushState rewrites plugin state properties; that is not a user-visible change
    edit_->flushState();
    collector_->endScope();
    manifest_.name = name();

    auto r = writeProject (dir_, edit_->state, manifest_, opt);

    if (r.ok())
    {
        dirty_ = false;
        manifest_.savedAt = nowIso8601();

        if (onPersisted)
            onPersisted (dir_, false);
    }

    return r;
}

Result<void> Project::autosave()
{
    collector_->beginScope();
    edit_->flushState();
    collector_->endScope();
    auto r = writeAutosave (dir_, edit_->state, manifest_);

    if (r.ok() && onPersisted)
        onPersisted (dir_ / "autosave" / (std::to_string (manifest_.revision) + ".rvx"), true);

    return r;
}

} // namespace rovox
