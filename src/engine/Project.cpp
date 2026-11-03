// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Project.h"

#include "PanStage.h"
#include "Transport.h"

#include <unistd.h>

namespace rovox
{

// Audio clips saved before the real-time wave node with the best resampler became the default carry neither property and would stay on
// the proxy path with the Lagrange interpolator (see RovoxEngineBehaviour::getClipDefaults).
static void defaultClipResampling (juce::ValueTree node)
{
    if (node.hasType (te::IDs::AUDIOCLIP))
    {
        if (! node.hasProperty (te::IDs::resamplingQuality))
            node.setProperty (te::IDs::resamplingQuality, juce::VariantConverter<te::ResamplingQuality>::toVar (te::ResamplingQuality::sincBest), nullptr);

        if (! node.hasProperty (te::IDs::proxyAllowed))
            node.setProperty (te::IDs::proxyAllowed, false, nullptr);
    }

    for (auto child : node)
        defaultClipResampling (child);
}

std::unique_ptr<te::Edit> createEditForProject (te::Engine& engine, const fs::path& dir, juce::ValueTree state, te::Edit::EditRole role)
{
    defaultClipResampling (state);
    te::Edit::Options o { engine };
    o.editState = std::move (state);
    o.editProjectItemID = te::ProjectItemID::createNewID (te::ProjectID {});
    o.role = role;
    o.numAudioTracks = 0;
    o.defaultMasterVolumedB = 0.0f;

    const auto editFile = toFile (dir / editFileName);
    o.editFileRetriever = [editFile] { return editFile; };
    auto edit = te::Edit::createEdit (std::move (o));

    if (edit != nullptr)
        installPanStages (*edit);       // REAPER pan law / pan mode on every track fader

    return edit;
}

Project::Project (EngineHost& host, fs::path dir, Manifest manifest, juce::ValueTree editState, PublishFn publish)
    : host_ (host), dir_ (std::move (dir)), manifest_ (std::move (manifest)), publish_ (std::move (publish)), revision_ (manifest_.revision)
{
    std::error_code ec;

    for (const char* sub : { "media", "renders", "peaks", "autosave" })
        fs::create_directories (dir_ / sub, ec);

    edit_ = createEditForProject (host.engine(), dir_, std::move (editState), te::Edit::EditRole::forEditing);

    // The command bus owns the undo transactions: one command is one step. Tracktion closes the open transaction 350 ms after a change, which on a
    // busy machine fell between a command and the asynchronous follow-up writes of the Edit (e.g. a plugin reordered after fx.add), so that
    // a single undo took back only the follow-up. Without it those writes join the transaction of the command that caused them.
    undoInhibitor_ = std::make_unique<te::Edit::UndoTransactionInhibitor> (*edit_);

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
    undoInhibitor_.reset();
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
