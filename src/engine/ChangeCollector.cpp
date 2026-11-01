// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ChangeCollector.h"

#include "Ids.h"
#include "ProjectFormat.h"

namespace rovox
{

json ChangedEntity::toJson() const
{
    return { { "kind", kind }, { "id", id }, { "op", op }, { "fields", json (fields) } };
}

const char* ChangeCollector::prefixFor (const juce::ValueTree& t)
{
    if (t.hasType (te::IDs::TRACK) || t.hasType (te::IDs::FOLDERTRACK))        return "trk";
    if (t.hasType (te::IDs::AUDIOCLIP) || t.hasType (te::IDs::MIDICLIP))       return "itm";
    if (t.hasType (te::IDs::MARKERCLIP))                                        return "mrk";
    if (t.hasType (te::IDs::PLUGIN))                                            return t["type"].toString() == "auxsend" ? "snd" : "fx";
    return nullptr;
}

const char* ChangeCollector::kindFor (const std::string& id)
{
    if (id.rfind ("trk_", 0) == 0)  return "track";
    if (id.rfind ("itm_", 0) == 0)  return "item";
    if (id.rfind ("mrk_", 0) == 0)  return "marker";
    if (id.rfind ("fx_", 0) == 0)   return "plugin";
    if (id.rfind ("snd_", 0) == 0)  return "send";
    return "edit";
}

void ChangeCollector::assignMissingIds (juce::ValueTree root)
{
    if (const char* prefix = prefixFor (root); prefix != nullptr && ! root.hasProperty (prop::id))
        root.setProperty (prop::id, juce::String (newId (prefix)), nullptr);

    for (auto child : root)
        assignMissingIds (child);
}

ChangeCollector::ChangeCollector (juce::ValueTree editState, SystemChangeFn fn) : state_ (std::move (editState)), onSystemChange_ (std::move (fn))
{
    state_.addListener (this);
}

ChangeCollector::~ChangeCollector()
{
    cancelPendingUpdate();
    state_.removeListener (this);
}

void ChangeCollector::beginScope()
{
    scopeActive_ = true;
    scopeChanges_.clear();
}

std::vector<ChangedEntity> ChangeCollector::endScope()
{
    scopeActive_ = false;
    std::vector<ChangedEntity> out;

    for (auto& [key, e] : scopeChanges_)
        out.push_back (std::move (e));

    scopeChanges_.clear();
    return out;
}

void ChangeCollector::record (const juce::ValueTree& tree, const juce::Identifier* property, const char* op)
{
    // nearest ancestor (or self) that is an entity with an id
    juce::ValueTree entity = tree;

    while (entity.isValid() && ! entity.hasProperty (prop::id))
        entity = entity.getParent();

    ChangedEntity change;
    change.op = op;

    if (entity.isValid())
    {
        change.id = entity[prop::id].toString().toStdString();
        change.kind = kindFor (change.id);
    }
    else
    {
        change.kind = "edit";
    }

    auto& map = scopeActive_ ? scopeChanges_ : systemChanges_;
    const auto key = change.kind + ":" + change.id;
    auto& slot = map.try_emplace (key, change).first->second;

    if (std::string_view (op) != "update" && slot.op == "update")
        slot.op = op;

    if (property != nullptr)
        slot.fields.insert (property->toString().toStdString());

    if (! scopeActive_)
        triggerAsyncUpdate();
}

void ChangeCollector::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p)
{
    if (p == te::IDs::lastSignificantChange || p == prop::id || p == prop::revision || p == prop::schema)
        return;

    // the playhead is written into the TRANSPORT node while the Edit plays: not a project change (it would bump the revision 30 times a second)
    if (p == te::IDs::position && t.hasType (te::IDs::TRANSPORT))
        return;

    record (t, &p, "update");
}

namespace
{
void collectIds (const juce::ValueTree& t, const juce::ValueTree& skip, std::set<std::string>& into)
{
    if (t == skip)
        return;

    if (t.hasProperty (prop::id))
        into.insert (t[prop::id].toString().toStdString());

    for (auto c : t)
        collectIds (c, skip, into);
}

/** A node copied from another one (Tracktion's splitClip, duplicate ...) carries the id of its original: give it a fresh one. */
void dropDuplicateIds (juce::ValueTree node, const std::set<std::string>& taken)
{
    if (node.hasProperty (prop::id) && taken.count (node[prop::id].toString().toStdString()) > 0)
        node.removeProperty (prop::id, nullptr);

    for (auto c : node)
        dropDuplicateIds (c, taken);
}
} // namespace

void ChangeCollector::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    {
        std::set<std::string> taken;
        collectIds (state_, child, taken);
        dropDuplicateIds (child, taken);
    }

    assignMissingIds (child);       // before recording, so that the event carries the new id

    if (prefixFor (child) != nullptr)
        record (child, nullptr, "add");
    else
        record (parent, nullptr, "update");
}

void ChangeCollector::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (prefixFor (child) != nullptr)
        record (child, nullptr, "remove");
    else
        record (parent, nullptr, "update");
}

void ChangeCollector::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    static const juce::Identifier order ("order");
    record (parent, &order, "update");
}

void ChangeCollector::discardPending()
{
    cancelPendingUpdate();
    systemChanges_.clear();
}

void ChangeCollector::handleAsyncUpdate()
{
    std::vector<ChangedEntity> out;

    for (auto& [key, e] : systemChanges_)
    {
        // Tracktion re-sorts track and clip lists asynchronously after an insert or an undo (spike P4 finding 2): not a user-visible
        // change. Deliberate reordering (track.move) happens inside a command scope and is reported by it.
        if (e.fields.size() == 1 && *e.fields.begin() == "order")
            continue;

        out.push_back (std::move (e));
    }

    systemChanges_.clear();

    if (! out.empty() && onSystemChange_)
        onSystemChange_ (std::move (out));
}

} // namespace rovox
