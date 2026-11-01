// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include <string>

namespace rovox
{

/** Rovox release version, e.g. "0.1.0". */
std::string rovoxVersion();

/** Git revision Rovox was built from, or "unknown". */
std::string rovoxGitSha();

/** Version of the pinned Tracktion Engine submodule. */
std::string tracktionVersion();

} // namespace rovox
