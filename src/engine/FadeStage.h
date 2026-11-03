// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER item fades. Tracktion's clip fades only know four fixed curves; a REAPER fade (shape 0..6 with a curve parameter, src/core/Fades.h)
// that is not the plain linear ramp is therefore kept on the clip ("rovox:fadeInLen" seconds, "rovox:fadeInShape", "rovox:fadeInCurve", the
// same with Out) and applied by a stage at the head of the track's plugin chain ("rovoxfades", before any FX, as REAPER applies item fades).
// The clip's own Tracktion fade is 0 then. The stage multiplies the sum of the track's items, so overlapping items on one track share the
// product of their fades (crossfades are not modelled).
#pragma once

#include "Fades.h"
#include "Tracktion.h"

namespace rovox
{

/** Registers the plugin type with the engine's plugin manager; call once after the Engine exists. */
void registerFadeStage (te::Engine&);

/** Sets a fade of a clip. A plain linear ramp is the clip's own fade, any other curve goes to the stage (which is installed on the clip's track). */
void setItemFade (te::Clip&, bool fadeIn, double lengthSeconds, const fades::FadeCurve&);

/** True for the item-fade stage. It is an implementation detail of the track chain: fx.list, project.get, the track JSON and the FX indices
    users give (fx.add index, fx.move to_index) leave it out, so the first FX users see is index 0. */
bool isFadeStage (te::Plugin&);

/** Position in the plugin list of the `visibleIndex`-th plugin that is not the fade stage (the list size when there are fewer). */
int chainIndexOfVisible (te::PluginList&, int visibleIndex);

/** Index of a plugin among the plugins that are not the fade stage. */
int visibleIndexOf (te::PluginList&, te::Plugin&);

/** The fade length of a clip in seconds, whichever of the two mechanisms holds it. */
double itemFadeSeconds (te::Clip&, bool fadeIn);

/** Drops the REAPER curve of a fade (the clip's own fade length stays what it is). */
void clearItemFadeCurve (te::Clip&, bool fadeIn);

} // namespace rovox
