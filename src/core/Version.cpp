// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Version.h"

namespace rovox
{

std::string rovoxVersion()    { return ROVOX_VERSION; }
std::string rovoxGitSha()     { return ROVOX_GIT_SHA; }
std::string tracktionVersion() { return ROVOX_TRACKTION_VERSION; }

} // namespace rovox
