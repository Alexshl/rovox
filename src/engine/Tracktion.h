// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// One place that pulls in Tracktion Engine (and, through it, the JUCE modules Rovox uses) and defines the shared aliases.
#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "Result.h"

#include <filesystem>
#include <string>

namespace rovox
{

namespace te = tracktion;
namespace fs = std::filesystem;

inline juce::File toFile (const fs::path& p)        { return juce::File (juce::CharPointer_UTF8 (p.string().c_str())); }
inline fs::path toPath (const juce::File& f)        { return fs::path (f.getFullPathName().toStdString()); }
inline std::string toStd (const juce::String& s)    { return s.toStdString(); }
inline juce::String toJuce (const std::string& s)   { return juce::String::fromUTF8 (s.c_str(), (int) s.size()); }

} // namespace rovox
