// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER .RPP import (task 150): tracks, folders, audio and MIDI items, markers/regions, tempo, aux sends.
// FX chains are not translated in M1: they are listed as unsupported and kept verbatim on the track (`rovox:unsupported_fx`).
#pragma once

#include "Project.h"

namespace rovox
{

struct RppImportOptions
{
    bool copyMedia = true;      // false: reference external files in place (absolute path, flagged `external` in the manifest)
};

struct RppImportReport
{
    int tracks = 0, folders = 0, items = 0, midiItems = 0, markers = 0, regions = 0, sends = 0;
    json unsupported = json::array();       // [{kind, name, track_id, ...}]
    json missingMedia = json::array();      // [{path, track_id, item}]
    json warnings = json::array();

    json toJson() const;
};

/** Message thread, inside a command (one undo step). The RPP text is parsed first; a broken file changes nothing. */
Result<RppImportReport> importRpp (Project&, const fs::path& rppFile, const RppImportOptions&);

/** Registers `import.rpp` in the command registry (called from registerImportCommands). */
class CommandRegistry;
void registerRppCommands (CommandRegistry&);

} // namespace rovox
