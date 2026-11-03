// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER-compatible panning for track faders. Tracktion's VolumeAndPanPlugin only knows its own (linear, 1+-pan) law; the track fader is
// therefore replaced by RovoxVolumeAndPan (plugin type "rovoxvolume", a VolumeAndPanPlugin subclass, so everything that talks to the fader keeps
// working and the pan value stays in the fader's `pan`). It pre-shapes the signal with the REAPER pan matrix (src/core/PanLaw.h) such that
// the Tracktion gains that follow come out as the REAPER result.
//
// Settings: the project (Edit state) holds the defaults "rovox:panMode" (REAPER PANMODE number, default 3) and "rovox:panLawDb" (<= 0, default 0);
// a track overrides them with the same properties on its fader plugin ("rovox:panMode" -1 = project default, "rovox:panLawDb" > 0 = project
// default). Dual pan keeps its second position in "rovox:panRight" of the fader (default +1, i.e. straight pass-through at pan -1).
#pragma once

#include "Tracktion.h"

namespace rovox
{

/** Registers the plugin type with the engine's plugin manager; call once after the Engine exists. */
void registerPanStage (te::Engine&);

/** Makes the track's fader a RovoxVolumeAndPan (keeping its state and id). No-op when it already is one. */
void installPanStage (te::AudioTrack&);

/** installPanStage for every audio track of the edit. */
void installPanStages (te::Edit&);

/** Project defaults (also what a new project gets: balance mode, 0 dB law). */
void setProjectPan (te::Edit&, int reaperPanMode, double lawDb);
int projectPanMode (te::Edit&);
double projectPanLawDb (te::Edit&);

/** Per-track override; mode < 0 = project default, lawDb > 0 = project default. */
void setTrackPan (te::AudioTrack&, int reaperPanMode, double lawDb);

} // namespace rovox
