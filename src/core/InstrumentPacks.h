// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Instrument packs: SoundFont (SF2 / SF3) collections played by the built-in "rovox:sf2" plugin. A pack is a directory <root>/<pack id>/ with pack.json
// and its soundfont file(s); the roots are the directories of $ROVOX_INSTRUMENTS_PATH (colon separated, default /instruments). One pack is built in: the
// General MIDI soundfont that the Docker image ships (MuseScore General Lite, MIT), listed when its file is installed.
//
// pack.json: {"id", "version", "sha256" (of the main soundfont file), "license" (SPDX), "server_use_ok" (true | false | "unknown"), "attribution", "source_url",
// "license_notes", "files" {"<name>": "<sha256>"}, "instruments" [{"name", "gm_program", "bank", "range" [low, high], "tags" []}]}.
// A pack whose server_use_ok is not exactly true is hidden from tenants that are restricted to free plugins (the licence gate of PluginCatalog).
#pragma once

#include "Result.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rovox
{

struct PackInstrument
{
    std::string name;
    int bank = 0, program = 0;                  // MIDI bank and program (gm_program) of the preset in the soundfont
    int lowNote = 0, highNote = 127;            // playable range
    std::vector<std::string> tags;
};

struct InstrumentPack
{
    std::string id, version, sha256, license, attribution, sourceUrl, licenseNotes;
    json serverUseOk = "unknown";               // true | false | "unknown"
    std::map<std::string, std::string> files;   // file name -> sha256 (empty for the built-in pack)
    std::vector<PackInstrument> instruments;
    std::filesystem::path dir;
    bool builtin = false;

    /** The main soundfont: the first .sf2 / .sf3 of `files` (by name). Empty path if there is none. */
    std::filesystem::path soundfontPath() const;
    /** True only if server_use_ok is exactly boolean true. */
    bool serverUse() const      { return serverUseOk.is_boolean() && serverUseOk.get<bool>(); }
};

struct PackScan
{
    std::vector<InstrumentPack> packs;
    json problems = json::array();              // [{"path", "error"}] for pack.json files that could not be used
};

class InstrumentPacks
{
public:
    static constexpr const char* builtinId = "gm-musescore-lite";

    /** The directories of $ROVOX_INSTRUMENTS_PATH, or "/instruments" (read on every call). */
    static std::vector<std::filesystem::path> searchPaths();
    /** Where the built-in soundfont is expected: $ROVOX_GM_SOUNDFONT or /usr/share/sounds/sf3/MuseScore_General_Lite.sf3. */
    static std::filesystem::path builtinSoundfont();

    /** Every pack below the search paths (a pack id that appears twice is taken from the first root) and the built-in one if its file exists. */
    static PackScan scan();
    static std::optional<InstrumentPack> find (const std::string& id);

    static Result<InstrumentPack> parse (const json& manifest, const std::filesystem::path& dir);
    static json toJson (const InstrumentPack&);

    /** Checks the pack on disk: every listed file exists and has its sha256, the declared sha256 is the one of the main soundfont
        (the built-in pack: its file exists and is not empty). {"id", "ok", "problems": []}. */
    static json verify (const InstrumentPack&);

    /** The sha256 that identifies the pack contents (the declared one; the built-in pack hashes its file once per size and mtime). */
    static std::string contentSha256 (const InstrumentPack&);
};

} // namespace rovox
