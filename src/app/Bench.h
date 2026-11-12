// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Cli.h"

namespace rovox::cli
{

/** `rovox bench`: generated projects (s1..s4) are opened from a saved file, rendered offline and played in real time through the
    hosted device interface, paced like a sound card. Prints a JSON report with --json, text otherwise. Exit codes: 0 done, 1 error. */
int runBench (const Options&);

} // namespace rovox::cli
