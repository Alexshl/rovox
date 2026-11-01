// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Tracktion.h"

#include <functional>
#include <map>
#include <set>
#include <vector>

namespace rovox
{

struct ChangedEntity
{
    std::string kind;               // track | item | plugin | marker | edit
    std::string id;                 // rovox:id ("" for the edit itself)
    std::string op = "update";      // add | update | remove
    std::set<std::string> fields;

    json toJson() const;
};

/** Assigns `rovox:id` to every track / clip / plugin / marker node of an Edit (also to nodes added later) and
    turns ValueTree changes into entity changes.
    - Inside a scope (a command is running) changes are collected and returned by endScope().
    - Outside a scope (Tracktion's own housekeeping, plugin loading) they are coalesced through an AsyncUpdater and
      handed to `onSystemChange` on the message thread, so the revision can advance without a command.
    Housekeeping properties are filtered out: `lastSignificantChange` (Tracktion stamps it after every real change,
    spike P4 finding) and our own `rovox:id` / `rovox:revision`. */
class ChangeCollector : private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    using SystemChangeFn = std::function<void (std::vector<ChangedEntity>)>;

    ChangeCollector (juce::ValueTree editState, SystemChangeFn onSystemChange);
    ~ChangeCollector() override;

    void beginScope();
    std::vector<ChangedEntity> endScope();
    bool inScope() const noexcept       { return scopeActive_; }

    /** Drops system changes that are queued but not yet reported (used once after load: Tracktion writes some
        parameter values into the tree asynchronously while an Edit initialises). */
    void discardPending();

    /** Assigns ids to everything in the tree that lacks one (used on load). Not undoable, no events. */
    static void assignMissingIds (juce::ValueTree root);

    static const char* prefixFor (const juce::ValueTree&);      // "trk", "itm", "fx", "mrk" or nullptr
    static const char* kindFor (const std::string& id);

private:
    void record (const juce::ValueTree& tree, const juce::Identifier* property, const char* op);
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void handleAsyncUpdate() override;

    juce::ValueTree state_;
    SystemChangeFn onSystemChange_;
    bool scopeActive_ = false;
    std::map<std::string, ChangedEntity> scopeChanges_, systemChanges_;
};

} // namespace rovox
