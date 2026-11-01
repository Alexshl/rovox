// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Version.h"

int main()
{
    return (rovox::rovoxVersion().empty() || rovox::rovoxGitSha().empty() || rovox::tracktionVersion().empty()) ? 1 : 0;
}
