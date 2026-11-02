// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Parameter values of a plugin are not part of the undoable tree (Tracktion writes an attached parameter back without an undo manager),
// so a command that changes them (fx.update, fx.preset_load) records an undo action of its own.
#pragma once

#include "Tracktion.h"

#include <map>

namespace rovox
{

struct SetParamsAction : juce::UndoableAction
{
    SetParamsAction (te::Plugin::Ptr p, std::map<std::string, float> b, std::map<std::string, float> a) : plugin (std::move (p)), before (std::move (b)), after (std::move (a)) {}

    bool perform() override     { apply (after); return true; }
    bool undo() override        { apply (before); return true; }

    void apply (const std::map<std::string, float>& values)
    {
        for (auto* ap : plugin->getAutomatableParameters())
            if (auto v = values.find (toStd (ap->paramID)); v != values.end())
                ap->setParameter (v->second, juce::sendNotification);
    }

    te::Plugin::Ptr plugin;
    std::map<std::string, float> before, after;
};

} // namespace rovox
