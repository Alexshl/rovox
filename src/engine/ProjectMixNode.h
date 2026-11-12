// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Live-playback gain / mute / solo of a project (ProjectMixerStrip). Installed as the last stage of the playback graph, so offline
// renders (which build their own graph) are not affected.
#pragma once

#include "Project.h"

#include <tracktion_graph/tracktion_graph.h>

namespace rovox
{

/** Last-stage node: output = input * gain, where gain follows the strip (mute, solo of a project of the same tenant, gain_db) with a ~15 ms linear slew. */
std::unique_ptr<tracktion::graph::Node> makeProjectMixNode (ProjectMixerStrip&, std::unique_ptr<tracktion::graph::Node> input);

} // namespace rovox
