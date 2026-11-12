// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The one table that says which scope(s) a command or MCP tool needs. The CommandBus and the MCP server consult it; a
// generative test fails when a registered command or tool has no entry.
#pragma once

#include "Auth.h"

#include <optional>
#include <string>
#include <vector>

namespace rovox
{

/** True if the name has an entry (prefix rules key.* and tenant.* included). */
bool hasScopeEntry (const std::string& name);

/** Scopes needed to call `name`, all of them. A name without an entry needs `admin` (fail closed).
    Empty for `batch` and `job.cancel`: those are checked per operation / per job kind by their callers. */
std::vector<Scope> requiredScopes (const std::string& name);

/** First required scope the principal lacks, nullopt if it may call `name`. */
std::optional<Scope> missingScope (const Principal&, const std::string& name);
bool allowed (const Principal&, const std::string& name);

/** Scope needed to act on a job of this kind (job.cancel): delivery -> delivery, import_plan -> projects:write, others -> render. */
Scope scopeForJobKind (const std::string& kind);

/** FORBIDDEN_SCOPE error naming the missing scope. */
Error forbiddenScopeError (Scope missing, const std::string& name);

} // namespace rovox
