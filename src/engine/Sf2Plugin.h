// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The built-in SoundFont instrument ("rovox:sf2", Tracktion plugin type "rovoxsf2"): plays a preset of an instrument pack (src/core/InstrumentPacks) with
// FluidSynth (LGPL-2.1+, linked dynamically). One synth per plugin instance, rendered on the calling thread with fluid_synth_write_float, reverb and
// chorus off, no threads of its own: the same MIDI gives the same samples. State (properties of the plugin state): "pack" (pack id), "bank", "program";
// automatable parameter: "gain" (dB). All MIDI channels play on one synth channel.
//
// Catalog entries of the pack presets have the plugin_uid "rovox:sf2/<pack id>/<bank>-<program>" (see sf2PresetUid).
#pragma once

#include "Tracktion.h"

#include <optional>

namespace rovox
{

inline constexpr const char* sf2XmlType = "rovoxsf2";

struct Sf2Preset
{
    std::string pack;
    int bank = 0, program = 0;
};

/** "rovox:sf2/<pack>/<bank>-<program>" */
std::string sf2PresetUid (const std::string& pack, int bank, int program);
/** True for "rovox:sf2" and anything below "rovox:sf2/". */
bool isSf2Uid (const std::string&);
/** The pack and preset of a "rovox:sf2/<pack>/<bank>-<program>" uid; nullopt for any other string. */
std::optional<Sf2Preset> parseSf2PresetUid (const std::string&);

/** Registers the plugin type with the engine's plugin manager; call once after the Engine exists. */
void registerSf2Plugin (te::Engine&);

/** A new SF2 plugin in the edit (not yet in a chain). The pack must be installed (NOT_FOUND otherwise); a preset the soundfont does not have is reported
    by the plugin state and the render preflight, not here. */
Result<te::Plugin::Ptr> createSf2Plugin (te::Edit&, const Sf2Preset&);

/** State of an SF2 plugin: {"pack", "bank", "program", "preset", "status": "ready|error", "error", "soundfont", "license", "attribution", "source_url", "pack_version"}.
    Null for other plugins. */
json sf2PluginExtras (te::Plugin&);

/** The pack an SF2 plugin plays: {"id", "version", "sha256", "license", "source"}; null for other plugins or when the pack is not installed. */
json sf2PackIdentity (te::Plugin&);

} // namespace rovox
